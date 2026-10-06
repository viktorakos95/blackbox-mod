/*
 * The live looper's page: Looper mode of the Mixer screen (MIX: Mixer -> Mute -> Solo -> Looper). Original 1010music
 * Blackbox, firmware 3.1.9. The engine is looper.c; solo.c routes the mixer view's hooks here while the mode shows.
 *
 * One strip per track, like the channel strips of a looper pedal, drawn over the 4 x 4 mixer cells:
 *   row 0     the track's box: tap / hold / double tap to record (the engine times the gestures), its colour is the
 *             state (dark empty, red recording, yellow overdubbing, green playing, white clearing, grey undoing),
 *             a white frame while a take is latched, the playhead along its bottom.
 *   row 1     three small buttons: PAN (tap: the track's knob turns its pan instead of its level; the bar shows L-R),
 *             REV (tap: play backwards), M (tap: mute; hold 2 s: undo the last pass; keep holding to 4 s: erase).
 *   rows 2-3  the fader: drag it, or turn the track's knob (knob 1..4 = track 1..4).
 * Text is drawn with the firmware's own string renderer (FUN_0808ee54, the one the pad names use).
 * For finding the INFO button: the page shows the last hardware button message (id:index) in its bottom-right corner.
 */
#include <stdint.h>

#include "looper.h"

#define FN(addr) ((addr) | 1u)

typedef void (*fill_fn)(const void *rect, int color, void *fb);
typedef void (*text_fn)(const char *s, const int *rect, int color, void *fb);
typedef int (*width_fn)(const char *s);
typedef int (*hit_fn)(void *view, const void *pt, uint8_t **cell);
typedef void (*msg_fn)(void *obj, const uint16_t *msg);

#define fw_fill     ((fill_fn)FN(0x0808ea22))
#define fw_outline  ((fill_fn)FN(0x0808e994))
#define fw_text     ((text_fn)FN(0x0808ee54))
#define fw_width    ((width_fn)FN(0x0808ed28))
#define fw_hit      ((hit_fn)FN(0x080b5e78))
#define fw_view_msg ((msg_fn)FN(0x080b5c70))     /* mixer view message handler (vtable +0x34) */
#define fw_app_msg  ((msg_fn)FN(0x080a2e60))     /* app message dispatch */

uint8_t *solo_looper_view(void);

#define VIEW_CELLS  0x3ac
#define CELL_SIZE   0x1a0
#define CELL_PAD    0x38         /* u16 pad id: bits 0-3 column, 4-7 row */
#define CELL_RECT   0x04         /* int x, y, w, h */
#define CELL_DIRTY  0x17c
#define CTX_FB      0x04

#define MSG_KNOB    0x32         /* view message: +0xc knob 0..3 (int16), +0x10 counts (int16) */
#define KNOB_SCALE  (1.f / 8000.f)

/* palette (table at 0x080f1d80) */
#define C_DARK   0x19
#define C_RAIL   0x10
#define C_GREY   0x09
#define C_LIGHT  0x16
#define C_WHITE  0x0f
#define C_BLACK  0x0e
#define C_GREEN  0x0b
#define C_RED    0x0c
#define C_REC    0x06
#define C_YELLOW 0x14
#define C_CYAN   0x1b
#define C_TEAL   0x1a

enum { P_NONE, P_REC, P_MUTE, P_FADER };

/* After the engine's state in the backup SRAM. Its first word is reset by looper_boot. */
struct page {
    int32_t _legacy;             /* step 2's fader drag; looper_boot writes -1 here */
    uint8_t pressed, track, pan_sel, _r;
    uint16_t btn_id, btn_index;  /* last hardware button message */
};
#define P ((volatile struct page *)0x38800f00u)

static uint8_t *cell_at(uint8_t *view, int i)
{
    return view + VIEW_CELLS + i * CELL_SIZE;
}

static int row_of(const uint8_t *cell)
{
    return (*(const uint16_t *)(cell + CELL_PAD) >> 4) & 0xf;
}

static int col_of(const uint8_t *cell)
{
    return *(const uint16_t *)(cell + CELL_PAD) & 0xf;
}

static const int *rect_of(uint8_t *view, int row, int col)
{
    for (int i = 0; i < 16; i++) {
        uint8_t *c = cell_at(view, i);
        if (row_of(c) == row && col_of(c) == col)
            return (const int *)(c + CELL_RECT);
    }
    return 0;
}

static void dirty(uint8_t *view)
{
    for (int i = 0; i < 16; i++)
        cell_at(view, i)[CELL_DIRTY] = 1;
}

static void box(void *fb, int x, int y, int w, int h, int color)
{
    int r[4] = {x, y, w, h};
    if (w > 0 && h > 0)
        fw_fill(r, color, fb);
}

static void frame(void *fb, int x, int y, int w, int h, int color)
{
    int r[4] = {x, y, w, h};
    if (w > 0 && h > 0)
        fw_outline(r, color, fb);
}

/* Centred text in a box. */
static void label(void *fb, int x, int y, int w, int h, const char *s, int color)
{
    int tw = fw_width(s);
    int r[4] = {x + (w - tw) / 2, y + (h - 14) / 2, tw + 2, 16};
    fw_text(s, r, color, fb);
}

static char *put_uint(char *p, unsigned v)
{
    char tmp[6];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 6);
    while (n)
        *p++ = tmp[--n];
    return p;
}

static char *put_hex2(char *p, unsigned v)
{
    static const char hex[] = "0123456789abcdef";
    *p++ = hex[(v >> 4) & 0xf];
    *p++ = hex[v & 0xf];
    return p;
}

static const char *mode_name(const struct looper_info *k)
{
    switch (k->mode) {
    case LOOPER_REC:
        return "REC";
    case LOOPER_DUB:
        return "DUB";
    case LOOPER_PLAY:
        return k->muted ? "MUTED" : "PLAY";
    case LOOPER_CLEARING:
        return "ERASE";
    case LOOPER_UNDOING:
        return "UNDO";
    default:
        return "EMPTY";
    }
}

static void draw_box(void *fb, const int *r, int t, const struct looper_info *k)
{
    int fill = k->mode == LOOPER_REC ? C_REC : k->mode == LOOPER_DUB ? C_YELLOW : k->mode == LOOPER_PLAY ? C_GREEN
             : k->mode == LOOPER_CLEARING ? C_WHITE : k->mode == LOOPER_UNDOING ? C_LIGHT : C_DARK;
    if (k->mode == LOOPER_PLAY && k->muted)
        fill = C_RAIL;
    box(fb, r[0], r[1], r[2], r[3], fill);
    int ink = fill == C_DARK || fill == C_RAIL || fill == C_REC ? C_WHITE : C_BLACK;
    char name[8] = {(char)('1' + t), 0};
    label(fb, r[0], r[1] + 2, r[2], r[3] / 2, name, ink);
    label(fb, r[0], r[1] + r[3] / 2 - 4, r[2], r[3] / 2, mode_name(k), ink);
    if (k->mode == LOOPER_PLAY || k->mode == LOOPER_DUB || k->mode == LOOPER_REC)
        box(fb, r[0], r[1] + r[3] - 4, (int)(looper_progress() * (float)r[2]), 4, C_CYAN);
    if (k->latched)
        frame(fb, r[0] + 1, r[1] + 1, r[2] - 2, r[3] - 2, C_WHITE);
}

static void draw_buttons(void *fb, const int *r, int t, const struct looper_info *k)
{
    int w = r[2] / 3, x = r[0], y = r[1], h = r[3];
    int has = k->mode == LOOPER_PLAY || k->mode == LOOPER_DUB;
    /* PAN */
    int sel = (P->pan_sel >> t) & 1;
    box(fb, x, y, w, h, sel ? C_TEAL : C_DARK);
    label(fb, x, y + 1, w, h / 2, "PAN", C_WHITE);
    int mid = x + w / 2, half = w / 2 - 4, by = y + h * 3 / 4;
    box(fb, x + 4, by, w - 8, 2, C_GREY);
    box(fb, mid - 1, by - 3, 2, 8, C_LIGHT);
    box(fb, mid + (int)(k->pan * (float)half) - 2, by - 4, 4, 10, C_CYAN);
    /* REV */
    box(fb, x + w, y, w, h, k->reversed ? C_YELLOW : C_DARK);
    label(fb, x + w, y, w, h, "REV", k->reversed ? C_BLACK : C_WHITE);
    /* MUTE */
    int mfill = !has ? C_DARK : k->muted ? C_RED : C_GREEN;
    box(fb, x + 2 * w, y, r[0] + r[2] - (x + 2 * w), h, mfill);
    label(fb, x + 2 * w, y, r[0] + r[2] - (x + 2 * w), h, "M", C_WHITE);
    frame(fb, x, y, r[2], h, C_GREY);
    box(fb, x + w, y, 1, h, C_GREY);
    box(fb, x + 2 * w, y, 1, h, C_GREY);
}

/* The fader spans rows 2 and 3; each cell draws its own part of it. */
static void draw_fader(uint8_t *view, void *fb, const int *r, int row, int t, const struct looper_info *k)
{
    const int *top = rect_of(view, 2, t), *bot = rect_of(view, 3, t);
    if (!top || !bot)
        return;
    int y0 = top[1] + 6, y1 = bot[1] + bot[3] - 6, span = y1 - y0;
    int cap = y1 - (int)(k->level * (float)span);
    box(fb, r[0], r[1], r[2], r[3], C_DARK);
    int cx = r[0] + r[2] / 2;
    /* clip every piece to this cell */
    int ct = r[1], cb = r[1] + r[3];
    int a = cap > ct ? cap : ct, b = y1 < cb ? y1 : cb;
    box(fb, cx - 2, ct > y0 ? ct : y0, 4, (cb < y1 ? cb : y1) - (ct > y0 ? ct : y0), C_RAIL);
    if (b > a)
        box(fb, cx - 6, a, 12, b - a, k->muted ? C_TEAL : C_CYAN);
    if (cap - 3 >= ct && cap + 3 <= cb)
        box(fb, r[0] + 6, cap - 3, r[2] - 12, 6, C_WHITE);
    if (row == 2) {
        char pct[8], *p = put_uint(pct, (unsigned)(k->level * 100.f + .5f));
        *p = 0;
        label(fb, r[0], r[1] + 2, r[2] / 3, 16, pct, C_LIGHT);
    } else if (t == LOOPER_TRACKS - 1) {
        char dbg[12] = "b", *p = put_hex2(dbg + 1, P->btn_id);
        *p++ = ':';
        p = put_uint(p, P->btn_index);
        *p = 0;
        label(fb, r[0] + r[2] / 2, r[1] + r[3] - 18, r[2] / 2, 16, dbg, C_GREY);
    }
}

/* Called for every mixer cell while the page shows; returns 0 to let the stock draw handle the cell. */
int looper_page_draw(uint8_t *view, uint8_t *cell, uint8_t *ctx)
{
    int t = col_of(cell), row = row_of(cell);
    if (t >= LOOPER_TRACKS || row > 3)
        return 0;
    void *fb = *(void **)(ctx + CTX_FB);
    const int *r = (const int *)(cell + CELL_RECT);
    struct looper_info k;
    looper_track(t, &k);
    if (row == 0)
        draw_box(fb, r, t, &k);
    else if (row == 1)
        draw_buttons(fb, r, t, &k);
    else
        draw_fader(view, fb, r, row, t, &k);
    return 1;
}

static void fader_to(uint8_t *view, int t, int y)
{
    const int *top = rect_of(view, 2, t), *bot = rect_of(view, 3, t);
    if (!top || !bot)
        return;
    int y0 = top[1] + 6, y1 = bot[1] + bot[3] - 6;
    looper_set_level(t, (float)(y1 - y) / (float)(y1 - y0));
}

void looper_page_down(uint8_t *view, const int *pt)
{
    uint8_t *cell = 0;
    P->pressed = P_NONE;
    if (!fw_hit(view, pt, &cell) || !cell)
        return;
    int t = col_of(cell), row = row_of(cell);
    if (t >= LOOPER_TRACKS || row > 3)
        return;
    const int *r = (const int *)(cell + CELL_RECT);
    P->track = (uint8_t)t;
    if (row == 0) {
        looper_event(t, LOOPER_EV_REC_DOWN);
        P->pressed = P_REC;
    } else if (row == 1) {
        int third = (pt[0] - r[0]) * 3 / (r[2] > 0 ? r[2] : 1);
        if (third <= 0) {
            P->pan_sel ^= (uint8_t)(1u << t);
        } else if (third == 1) {
            looper_event(t, LOOPER_EV_REVERSE);
        } else {
            looper_event(t, LOOPER_EV_MUTE_DOWN);
            P->pressed = P_MUTE;
        }
    } else {
        fader_to(view, t, pt[1]);
        P->pressed = P_FADER;
    }
    dirty(view);
}

void looper_page_move(uint8_t *view, const int *pt)
{
    if (P->pressed == P_FADER) {
        fader_to(view, P->track, pt[1]);
        dirty(view);
    }
}

void looper_page_up(uint8_t *view, const int *pt)
{
    (void)pt;
    if (P->pressed == P_REC)
        looper_event(P->track, LOOPER_EV_REC_UP);
    else if (P->pressed == P_MUTE)
        looper_event(P->track, LOOPER_EV_MUTE_UP);
    P->pressed = P_NONE;
    dirty(view);
}

/* Mixer view vtable +0x34 (0x080f0f44): its messages. On the page the four knobs move the tracks' faders (or pans). */
void looper_view_msg(uint8_t *view, const uint16_t *msg)
{
    if (msg && msg[0] == MSG_KNOB && solo_looper_view() == view) {
        int knob = *(const int16_t *)((const uint8_t *)msg + 0xc);
        int counts = *(const int16_t *)((const uint8_t *)msg + 0x10);
        if (knob >= 0 && knob < LOOPER_TRACKS) {
            struct looper_info k;
            looper_track(knob, &k);
            if ((P->pan_sel >> knob) & 1)
                looper_set_pan(knob, k.pan + (float)counts * KNOB_SCALE * 2.f);
            else
                looper_set_level(knob, k.level + (float)counts * KNOB_SCALE);
            dirty(view);
        }
        return;
    }
    fw_view_msg(view, msg);
}

/* Replaces the app's message dispatch call (bl @0x080a23cc): note hardware button messages for the INFO hunt. */
void looper_app_msg(void *app, const uint16_t *msg)
{
    if (msg && msg[0] >= 0xf0 && msg[0] <= 0xff && looper_ready()) {
        P->btn_id = msg[0];
        P->btn_index = (uint16_t)*(const int32_t *)((const uint8_t *)msg + 0xc);
    }
    fw_app_msg(app, msg);
}
