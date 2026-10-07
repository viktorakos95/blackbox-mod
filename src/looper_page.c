/*
 * The live looper's page: Looper mode of the Mixer screen (MIX: Mixer -> Mute -> Solo -> Looper). Original 1010music
 * Blackbox, firmware 3.1.9. The engine is looper.c; solo.c routes the mixer view's hooks here while the mode shows.
 *
 * Look: thin lines, like the stock EQ screen (a 1 px frame, hairline curves, small corner text, round dots). One
 * frame around the page, a row of four values on top (what each knob is turning: LVL 74, or an L - R pan line), then
 * four columns divided by hairlines, then a footer with the loop length / position and the INFO button:
 *   record box  an outlined box (colour = state: red recording, yellow overdubbing, green playing; double line
 *               while a take is latched) with the track number at 2x, the state, an outlined icon and a thin
 *               playhead along its bottom. It is the record button: hold = record while held, double tap =
 *               latch, tap = keep (the engine times the gestures).
 *   fader       a hairline with ticks and a round handle (the white dot), a 2 px line in the track colour up to
 *               it, and a thin meter on each side for the left and right output (level and pan together).
 *               Touch or drag anywhere in it, or turn the track's knob. The level is written below.
 *   pan dial    drawn like the pad config's knobs (label, arc, pointer); tap it (pink frame) and the track's knob
 *               turns the pan instead of the level; tap again to give the knob back.
 *   buttons     REV (play backwards) | MUTE (tap: mute; hold 2 s: undo the last pass, 4 s: erase).
 * INFO is a modifier: press it (tap: stays on; held 0.45 s or more: only while held) and the knobs turn the pan, a
 * tap on a record box mutes, holding it undoes / erases (2 s / 4 s). The first press of any button other than MIX is
 * taken as INFO and remembered; the footer shows "INFO=n" once known.
 *
 * Drawing. The firmware's screen API has its origin at the bottom left: y grows upward and a rectangle's y is its
 * bottom edge (the low-level fill 0x08041c68 computes H - y - h, the pixel plot 0x0808f524 H - y - 1). The page is laid
 * out top-down on an area as wide as the screen (320 px; 256 px for the cells) and as tall as the mixer's 16 cells,
 * then mapped with GX / GY. (Step 4 assumed 476 px from a guess and would have drawn past the edge: never size
 * anything from a guess, only from the cells and the frame buffer.) Which way is up is
 * read from the cells themselves (pad row 3 is the top row, row 0 the bottom) and x from the columns, so a flipped
 * screen would still come out right. Text is the firmware's own 6x8 font (FUN_0808ee54 draws it at 2x, too big for
 * four columns) plotted pixel by pixel at 1x or 2x.
 *
 * State lives in the backup SRAM after the engine's (0x38800f00, 256 bytes). looper_boot calls page_boot().
 */
#include <stdint.h>

#include "looper.h"

#define FN(addr) ((addr) | 1u)

typedef void (*fill_fn)(const void *rect, int color, void *fb);
typedef void (*px_fn)(void *fb, int x, int y, int color);
typedef void (*msg_fn)(void *obj, const uint16_t *msg);

#define fw_fill     ((fill_fn)FN(0x0808ea22))
#define fw_px       ((px_fn)FN(0x0808f524))                /* fb, x, y (y up), palette index */
#define fw_view_msg ((msg_fn)FN(0x080b5c70))               /* mixer view message handler (vtable +0x34) */
#define fw_app_msg  ((msg_fn)FN(0x080a2e60))               /* app message dispatch */

uint8_t *solo_looper_view(void);

#define VIEW_CELLS  0x3ac
#define CELL_SIZE   0x1a0
#define CELL_PAD    0x38         /* u16 pad id: bits 0-3 column, 4-7 row */
#define CELL_RECT   0x04         /* int x, y, w, h */
#define CELL_DIRTY  0x17c
#define CTX_FB      0x04

#define FONT        ((const struct font *)0x240000d0u)     /* the firmware's text font: {glyphs, w, h} = 6 x 8 */
struct font {
    const uint8_t *data;
    uint16_t w, h;
};

#define MSG_KNOB    0x32         /* view message: +0xc knob 0..3 (int16), +0x10 counts (int16) */
#define KNOB_SCALE  (1.f / 8000.f)
#define MSG_BUTTON  0xf9         /* app message: +0xc = button 0..7 (5 = MIX) */
#define MSG_RELEASE 0xfa
#define BTN_MIX     5

/* palette (table at 0x080f1d80) */
#define C_BG     0x02            /* 141414: the screen */
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
#define C_TEAL   0x1a
#define C_PINK   0x20            /* the stock selection frame (the active knob bank in the pad config) */
#define C_TAB    0x01            /* 56565a: the stock tab buttons, which are textured on the real screen */
#define C_CYAN   0x1b            /* the selected tab */
static const uint8_t track_colour[LOOPER_TRACKS] = {0x1b, 0x14, 0x17, 0x18};   /* cyan, yellow, aqua, purple */

/* layout, in pixels, top-down. The page width and origin come from the screen (geometry()): about 314 px on the
 * 320 px wide display, the 64 px cells being centred with 32 px of margin on each side. */
#define MARGIN    3
#define GAP       1              /* a hairline between columns */
#define TOPBAR    14             /* the row of values; its line is at d = TOPBAR + 1 */
#define FOOT      12             /* the footer; its line is at d = hg - FOOT - 1 */
#define REC_H     40             /* the record box */
#define BTN       34             /* the bottom row: the pan dial and the REV / MUTE buttons */
#define DIAL      34             /* width of the pan dial's box */

#define INFO_TIMEOUT 1875        /* audio blocks (10 s) */
#define INFO_HOLD    84          /* 0.45 s */

enum { P_NONE, P_REC, P_MUTE, P_FADER };
enum { Z_NONE, Z_REC, Z_FADER, Z_PAN, Z_REV, Z_MUTE };

struct page {
    int32_t _legacy;             /* step 2's fader drag; looper_boot used to write -1 here */
    uint8_t pressed, track, pan_sel, info_on;
    uint8_t info_idx, info_set, entered, _r;     /* info_idx 0xff = not learned */
    uint16_t btn_id, btn_index;
    uint32_t magic, info_t, info_down, sig, force, fb;
    int32_t ytop, hg, s, sx;     /* geometry read from the cells at the last draw */
    int32_t x0, w;               /* the page's left edge and width, in fill space */
};
#define P ((volatile struct page *)0x38800f00u)
#define PMAGIC 0x50414731u

_Static_assert(sizeof(struct page) <= 0x100, "page state must fit its 256 bytes");

#define FB ((void *)P->fb)       /* the frame buffer (no .bss in the code cave: it lives with the page state) */

/* ---- geometry */

static uint8_t *cell_at(uint8_t *view, int i)
{
    return view + VIEW_CELLS + i * CELL_SIZE;
}

/* Read where the cells are: the page spans their rows, and which way is up follows from pad rows 0 and 3. */
static void geometry(uint8_t *view)
{
    int y_lo = 0x7fffffff, y_hi = -0x7fffffff, y_row0 = 0, y_row3 = 0, x_col0 = 0, x_col3 = 0;
    int x_lo = 0x7fffffff, x_hi = -0x7fffffff;
    for (int i = 0; i < 16; i++) {
        uint8_t *c = cell_at(view, i);
        const int *r = (const int *)(c + CELL_RECT);
        unsigned pad = *(uint16_t *)(c + CELL_PAD), row = (pad >> 4) & 0xf, col = pad & 0xf;
        if (r[1] < y_lo)
            y_lo = r[1];
        if (r[1] + r[3] > y_hi)
            y_hi = r[1] + r[3];
        if (r[0] < x_lo)
            x_lo = r[0];
        if (r[0] + r[2] > x_hi)
            x_hi = r[0] + r[2];
        if (row == 0)
            y_row0 = r[1];
        if (row == 3)
            y_row3 = r[1];
        if (col == 0)
            x_col0 = r[0];
        if (col == 3)
            x_col3 = r[0];
    }
    P->s = y_row3 >= y_row0 ? 1 : -1;
    P->sx = x_col3 >= x_col0 ? 1 : -1;
    P->hg = y_hi - y_lo;
    P->ytop = P->s > 0 ? y_hi : y_lo;
    if (P->hg < 120)
        P->hg = 120;
    /* The cells are centred on the screen, so the screen is as wide as the cells plus their margin twice; the frame
     * buffer's own width (fb + 4) caps it when it makes sense. Never wider than that: a fill past the edge is unsafe. */
    int ws = x_lo + x_hi, fbw = *(const uint16_t *)((const uint8_t *)FB + 4);
    if (fbw >= 160 && fbw <= 1024 && fbw < ws)
        ws = fbw;
    if (ws < x_hi || ws > 1024 || x_hi - x_lo < 80) {              /* not centred, or nothing sensible: the cells only */
        P->x0 = x_lo;
        P->w = x_hi - x_lo;
    } else {
        P->x0 = MARGIN;
        P->w = ws - 2 * MARGIN;
    }
}

static inline int GX(int x, int w)
{
    return P->sx > 0 ? P->x0 + x : P->x0 + P->w - x - w;
}

static inline int GY(int d, int h)
{
    return P->s > 0 ? P->ytop - d - h : P->ytop + d;
}

struct lay {
    int cx, cw;                  /* the column (x from the page's left edge) */
    int col_y, col_h;
    int rec_y, rec_h;            /* the record box */
    int bars_y, bars_h;          /* the fader's travel */
    int lvl_y, btn_y;
    int foot_y;                  /* the footer's line */
};

static void layout(int t, struct lay *L)
{
    int cw = (P->w - 2 - 3 * GAP) / 4, hg = P->hg;
    L->cw = cw;
    L->cx = 1 + t * (cw + GAP);
    L->col_y = TOPBAR + 2;
    L->col_h = hg - L->col_y - FOOT - 2;
    L->rec_y = L->col_y + 3;
    L->rec_h = REC_H;
    L->btn_y = L->col_y + L->col_h - 2 - BTN;
    L->lvl_y = L->btn_y - 12;
    L->bars_y = L->rec_y + L->rec_h + 6;
    L->bars_h = L->lvl_y - 4 - L->bars_y;
    if (L->bars_h < 20)
        L->bars_h = 20;
    L->foot_y = hg - FOOT - 1;
}

/* Which control is at x pixels from the left and d from the top, and the fader value there (0..1). */
int looper_page_hit(int x, int d, int *track, float *val)
{
    struct lay L;
    int cw = (P->w - 2 - 3 * GAP) / 4;
    int t = cw > 0 && x >= 1 ? (x - 1) / (cw + GAP) : -1;
    if (P->hg < 120 || P->w < 80 || t < 0 || t >= LOOPER_TRACKS || x - 1 - t * (cw + GAP) >= cw)
        return Z_NONE;                                            /* (hg is 0 until the page has been drawn once) */
    layout(t, &L);
    *track = t;
    if (d >= L.rec_y - 2 && d < L.rec_y + L.rec_h + 2)
        return Z_REC;
    if (d >= L.rec_y + L.rec_h + 3 && d < L.btn_y - 1) {
        float v = (float)(L.bars_y + L.bars_h - d) / (float)L.bars_h;
        *val = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
        return Z_FADER;
    }
    if (d >= L.btn_y && d < L.btn_y + BTN) {
        if (x - L.cx - 2 < DIAL)
            return Z_PAN;
        return d < L.btn_y + BTN / 2 ? Z_REV : Z_MUTE;
    }
    return Z_NONE;
}

/* ---- drawing */


static void box(int x, int d, int w, int h, int color)
{
    int r[4] = {GX(x, w), GY(d, h), w, h};
    if (w > 0 && h > 0)
        fw_fill(r, color, FB);
}

static void frame(int x, int d, int w, int h, int color, int th)
{
    box(x, d, w, th, color);
    box(x, d + h - th, w, th, color);
    box(x, d + th, th, h - 2 * th, color);
    box(x + w - th, d + th, th, h - 2 * th, color);
}

static int text_w(const char *s, int scale)
{
    int n = 0;
    while (s[n])
        n++;
    return n * FONT->w * scale;
}

/* The firmware's 6x8 font at scale 1 or 2, top-left at x / d. */
static void text(int x, int d, const char *s, int color, int scale)
{
    const struct font *f = FONT;
    if (!f->data || f->w != 6 || f->h != 8)
        return;
    for (; *s; s++, x += f->w * scale) {
        unsigned c = (unsigned char)*s;
        const uint8_t *g = f->data + (c < 0x20 || c > 0x7e ? '?' - 0x20 : c - 0x20) * f->h;
        for (int r = 0; r < f->h; r++)
            for (int k = 0; k < f->w; k++)
                if (g[r] & (0x80 >> k))
                    for (int a = 0; a < scale; a++)
                        for (int b = 0; b < scale; b++)
                            fw_px(FB, GX(x + k * scale + a, 1), GY(d + r * scale + b, 1), color);
    }
}

static void text_c(int x, int d, int w, const char *s, int color, int scale)
{
    text(x + (w - text_w(s, scale)) / 2, d, s, color, scale);
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

static const char *state_name(const struct looper_info *k)
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

static void hline(int x, int d, int w, int color)
{
    box(x, d, w, 1, color);
}

static void vline(int x, int d, int h, int color)
{
    box(x, d, 1, h, color);
}

/* A round dot, like the EQ's handles: 7 px across. */
static void dot(int cx, int cy, int color)
{
    box(cx - 1, cy - 3, 3, 7, color);
    box(cx - 2, cy - 2, 5, 5, color);
    box(cx - 3, cy - 1, 7, 3, color);
}

static const int8_t ring_pts[16][2] = {{8, 0}, {7, -3}, {6, -6}, {3, -7}, {0, -8}, {-3, -7}, {-6, -6}, {-7, -3},
                                       {-8, 0}, {-7, 3}, {-6, 6}, {-3, 7}, {0, 8}, {3, 7}, {6, 6}, {7, 3}};

static void ring(int cx, int cy, int color)
{
    for (int i = 0; i < 16; i++)
        box(cx + ring_pts[i][0], cy + ring_pts[i][1], 1, 1, color);
}

/* The state, drawn in outline at the bottom of the record box. */
static void icon(const struct lay *L, const struct looper_info *k, int color)
{
    int cx = L->cx + L->cw / 2, cy = L->rec_y + 30;
    switch (k->mode) {
    case LOOPER_REC:
    case LOOPER_DUB:
        ring(cx, cy, color);
        box(cx - 3, cy - 3, 6, 6, color);
        break;
    case LOOPER_PLAY:
        if (k->muted) {                                           /* two hairlines */
            vline(cx - 4, cy - 7, 14, color);
            vline(cx + 3, cy - 7, 14, color);
        } else {                                                  /* a triangle outline */
            vline(cx - 5, cy - 8, 17, color);
            for (int i = 0; i <= 8; i++) {
                box(cx - 5 + i * 11 / 8, cy - 8 + i, 1, 1, color);
                box(cx - 5 + i * 11 / 8, cy + 8 - i, 1, 1, color);
            }
        }
        break;
    case LOOPER_CLEARING:
    case LOOPER_UNDOING:
        for (int i = 0; i < 3; i++)
            box(cx - 8 + i * 8, cy - 1, 2, 2, color);
        break;
    default:
        break;
    }
}

/* The dial's arc: 28 points, 270 degrees from bottom left over the top to bottom right, radius 11. */
static const int8_t arc[28][2] = {
    {-8, 8}, {-9, 6}, {-10, 5}, {-11, 3}, {-11, 1}, {-11, -1}, {-11, -3}, {-10, -5}, {-9, -6}, {-8, -8},
    {-6, -9}, {-5, -10}, {-3, -11}, {-1, -11}, {1, -11}, {3, -11}, {5, -10}, {6, -9}, {8, -8}, {9, -6},
    {10, -5}, {11, -3}, {11, -1}, {11, 1}, {11, 3}, {10, 5}, {9, 6}, {8, 8}};

/* A knob like the pad config's: label, hairline arc, pointer at v (0..1). The box is DIAL wide and BTN high. */
static void dial(int x, int d, const char *name, float v, int selected)
{
    frame(x, d, DIAL, BTN, selected ? C_PINK : C_RAIL, 1);
    if (selected)
        frame(x + 1, d + 1, DIAL - 2, BTN - 2, C_PINK, 1);
    text_c(x, d + 3, DIAL, name, C_LIGHT, 1);
    int cx = x + DIAL / 2, cy = d + 22;
    for (int i = 0; i < 28; i++)
        box(cx + arc[i][0], cy + arc[i][1], 1, 1, C_LIGHT);
    int i = (int)(v * 27.f + .5f);
    i = i < 0 ? 0 : i > 27 ? 27 : i;
    for (int k = 1; k <= 9; k++)                                  /* the pointer, from the centre */
        box(cx + arc[i][0] * k / 11, cy + arc[i][1] * k / 11, 1, 1, C_CYAN);
}

static void draw_top(int t, const struct lay *L, const struct looper_info *k)
{
    int x = L->cx, w = L->cw, on_pan = ((P->pan_sel >> t) & 1) || P->info_on;
    box(x, 1, w, TOPBAR, C_BG);
    if (on_pan) {
        int half = (w - 24) / 2, mid = x + w / 2, c = P->info_on ? C_PINK : C_CYAN;
        text(x + 3, 4, "L", C_LIGHT, 1);
        text(x + w - 9, 4, "R", C_LIGHT, 1);
        hline(mid - half, 7, 2 * half, C_RAIL);
        vline(mid, 4, 7, C_GREY);
        dot(mid + (int)(k->pan * (float)half), 8, c);
    } else {
        char b[12] = "LVL ", *p = b + 4;
        p = put_uint(p, (unsigned)(k->level * 100.f + .5f));
        *p = 0;
        text_c(x, 4, w, b, C_WHITE, 1);
    }
}

static void draw_column(int t, const struct lay *L, const struct looper_info *k)
{
    int x = L->cx, w = L->cw, colour = track_colour[t];
    int live = k->mode == LOOPER_PLAY || k->mode == LOOPER_DUB;
    int scol = k->mode == LOOPER_REC ? C_REC : k->mode == LOOPER_DUB ? C_YELLOW : k->mode == LOOPER_PLAY ? (k->muted ? C_RED : C_GREEN)
             : k->mode == LOOPER_EMPTY ? C_RAIL : C_WHITE;
    box(x, L->col_y, w, L->col_h, C_BG);
    /* the record box */
    int rx = x + 3, rw = w - 6, ry = L->rec_y, rh = L->rec_h;
    frame(rx, ry, rw, rh, scol, 1);
    if (k->latched || k->mode == LOOPER_REC)
        frame(rx + 2, ry + 2, rw - 4, rh - 4, k->latched ? C_WHITE : scol, 1);     /* a second line, like a pressed button */
    char num[2] = {(char)('1' + t), 0};
    text(rx + 7, ry + 5, num, k->mode == LOOPER_EMPTY ? C_GREY : colour, 2);
    text(rx + 24, ry + 9, state_name(k), k->mode == LOOPER_EMPTY ? C_GREY : scol, 1);
    icon(L, k, scol);
    if (live || k->mode == LOOPER_REC) {                          /* the playhead, a hairline along the bottom */
        int px = (int)(looper_progress() * (float)(rw - 4));
        hline(rx + 2, ry + rh - 4, px, colour);
        dot(rx + 2 + px, ry + rh - 4, colour);
    }
    /* the fader: a hairline with ticks, a 2 px line in the track colour up to a round handle */
    int d = L->bars_y, h = L->bars_h, fx = x + w / 2;
    float gl = k->pan > 0.f ? 1.f - k->pan : 1.f, gr = k->pan < 0.f ? 1.f + k->pan : 1.f;
    vline(fx, d, h, C_RAIL);
    for (int q = 0; q <= 4; q++)
        hline(fx - 3, d + q * (h - 1) / 4, 7, C_GREY);
    int hy = d + h - (int)(k->level * (float)h + .5f);
    int lc = k->muted ? C_GREY : colour;
    box(fx, hy, 2, d + h - hy, lc);
    for (int c = 0; c < 2; c++) {                                 /* a thin meter for each output */
        int mx = c ? x + w - 9 : x + 8;
        int m = live ? (int)(k->level * (c ? gr : gl) * (float)h + .5f) : 0;
        vline(mx, d, h, C_RAIL);
        vline(mx, d + h - m, m, lc);
    }
    dot(fx, hy, C_WHITE);
    /* the level, small */
    char lv[8], *q = put_uint(lv, (unsigned)(k->level * 100.f + .5f));
    *q++ = '%';
    *q = 0;
    text_c(x, L->lvl_y, w, lv, k->muted ? C_GREY : C_LIGHT, 1);
    /* the pan dial and the buttons */
    int by = L->btn_y, dx = x + 2, rbw = w - 4 - DIAL - 1, bx = dx + DIAL + 1, hh = BTN / 2;
    dial(dx, by, "PAN", (k->pan + 1.f) * .5f, ((P->pan_sel >> t) & 1) != 0);
    int rc = k->reversed ? C_YELLOW : C_GREY;
    frame(bx, by, rbw, hh, rc, 1);
    text_c(bx, by + 5, rbw, "REV", k->reversed ? C_YELLOW : C_LIGHT, 1);
    int mc = !live ? C_RAIL : k->muted ? C_RED : C_GREEN;
    frame(bx, by + hh, rbw, BTN - hh, mc, 1);
    text_c(bx, by + hh + 5, rbw, "MUTE", !live ? C_GREY : mc, 1);
}

static void draw_footer(const struct lay *L)
{
    box(1, L->foot_y + 1, P->w - 2, FOOT - 1, C_BG);
    char b[48], *p = b;
    if (looper_len()) {
        unsigned len = looper_len() * 10u / 48000u, pos = (unsigned)(looper_progress() * (float)looper_len()) * 10u / 48000u;
        const char *s = "LOOP ";
        while (*s)
            *p++ = *s++;
        p = put_uint(p, len / 10);
        *p++ = '.';
        p = put_uint(p, len % 10);
        s = "s  POS ";
        while (*s)
            *p++ = *s++;
        p = put_uint(p, pos / 10);
        *p++ = '.';
        p = put_uint(p, pos % 10);
        *p++ = 's';
    } else {
        const char *s = "HOLD A TRACK TO RECORD";
        while (*s)
            *p++ = *s++;
    }
    *p = 0;
    text(5, L->foot_y + 3, b, C_LIGHT, 1);
    if (P->info_idx == 0xff) {
        text(P->w - 5 - text_w("PRESS INFO ONCE", 1), L->foot_y + 3, "PRESS INFO ONCE", C_PINK, 1);
    } else {
        char i[12] = "INFO=", *q = i + 5;
        q = put_uint(q, P->info_idx);
        *q = 0;
        text(P->w - 5 - text_w(i, 1), L->foot_y + 3, i, P->info_on ? C_PINK : C_GREY, 1);
    }
}

/* A number that changes whenever something the page shows changes. */
static uint32_t signature(void)
{
    uint32_t h = 2166136261u;
    for (int t = 0; t < LOOPER_TRACKS; t++) {
        struct looper_info k;
        looper_track(t, &k);
        uint32_t v = (uint32_t)k.mode | (uint32_t)k.muted << 4 | (uint32_t)k.reversed << 5 | (uint32_t)k.latched << 6 |
                     (uint32_t)(k.level * 100.f + .5f) << 8 | (uint32_t)(k.pan * 100.f + 100.5f) << 16;
        h = (h ^ v) * 16777619u;
    }
    h = (h ^ ((uint32_t)P->pan_sel | (uint32_t)P->info_on << 8 | (uint32_t)P->info_idx << 16)) * 16777619u;
    h = (h ^ (uint32_t)(looper_progress() * 400.f)) * 16777619u;     /* the playhead, in 1/400ths of a loop */
    return h ^ looper_len();
}

/* Called for every mixer cell while the page shows: the first cell draws the whole page, the others nothing.
 * Returns 0 only for a cell outside the grid (the stock draw then runs). */
int looper_page_draw(uint8_t *view, uint8_t *cell, uint8_t *ctx)
{
    int idx = (int)((cell - cell_at(view, 0)) / CELL_SIZE);
    if (idx < 0 || idx > 15)
        return 0;
    if (idx != 0)
        return 1;
    P->fb = (uint32_t)*(void **)(ctx + CTX_FB);
    geometry(view);
    if (!P->entered) {
        P->entered = 1;
        box(0, 0, P->w, P->hg, C_BG);
    }
    struct lay L;
    layout(0, &L);
    int fc = P->info_on ? C_PINK : C_LIGHT;                       /* the frame goes pink while INFO is on */
    frame(0, 0, P->w, P->hg, fc, 1);
    hline(1, TOPBAR + 1, P->w - 2, C_RAIL);
    hline(1, L.foot_y, P->w - 2, C_RAIL);
    for (int t = 0; t < LOOPER_TRACKS; t++) {
        struct looper_info k;
        looper_track(t, &k);
        layout(t, &L);
        draw_top(t, &L, &k);
        draw_column(t, &L, &k);
        if (t < LOOPER_TRACKS - 1)
            vline(L.cx + L.cw, 1, L.foot_y - 1, C_RAIL);
    }
    draw_footer(&L);
    P->sig = signature();
    return 1;
}

static void dirty(uint8_t *view)
{
    for (int i = 0; i < 16; i++)
        cell_at(view, i)[CELL_DIRTY] = 1;
    P->force = 0;
}

/* From the audio task (looper_ui_poke -> solo.c): redraw when something changed, and now and then regardless. */
void looper_page_poke(uint8_t *view)
{
    if (P->info_on && looper_ticks() - P->info_t > INFO_TIMEOUT) {
        P->info_on = 0;
        P->sig = 0;
    }
    if (signature() != P->sig || ++P->force >= 40)
        dirty(view);
}

/* The Looper page has just been (re)shown. */
void looper_page_enter(void)
{
    P->entered = 0;
    P->pressed = P_NONE;
    P->info_on = 0;
    P->sig = 0;
    P->force = 0;
}

/* looper_boot: reset the page; the learned INFO button is kept while the backup SRAM is. */
void looper_page_boot(void)
{
    if (P->magic != PMAGIC) {
        P->magic = PMAGIC;
        P->info_idx = 0xff;
    }
    P->_legacy = -1;
    P->hg = 0;
    P->pan_sel = 0;
    P->btn_id = P->btn_index = 0;
    looper_page_enter();
}

/* ---- touch and knobs */

/* Touch points use the same space as the cells: convert to pixels from the page's left and top. */
static void to_page(const int *pt, int *x, int *d)
{
    *x = P->sx > 0 ? pt[0] - P->x0 : P->x0 + P->w - pt[0];
    *d = P->s > 0 ? P->ytop - pt[1] : pt[1] - P->ytop;
}

static void info_touch(void)
{
    P->info_t = looper_ticks();
}

void looper_page_down(uint8_t *view, const int *pt)
{
    int x, d, t = 0;
    float v = 0.f;
    P->pressed = P_NONE;
    to_page(pt, &x, &d);
    int zone = looper_page_hit(x, d, &t, &v);
    if (zone == Z_NONE)
        return;
    P->track = (uint8_t)t;
    if (P->info_on)
        info_touch();
    if (zone == Z_REC) {
        if (P->info_on) {                                         /* INFO + box: mute, hold = undo / erase */
            looper_event(t, LOOPER_EV_MUTE_DOWN);
            P->pressed = P_MUTE;
        } else {
            looper_event(t, LOOPER_EV_REC_DOWN);
            P->pressed = P_REC;
        }
    } else if (zone == Z_FADER) {
        looper_set_level(t, v);
        P->pressed = P_FADER;
    } else if (zone == Z_PAN) {
        P->pan_sel ^= (uint8_t)(1u << t);
    } else if (zone == Z_REV) {
        looper_event(t, LOOPER_EV_REVERSE);
    } else {
        looper_event(t, LOOPER_EV_MUTE_DOWN);
        P->pressed = P_MUTE;
    }
    dirty(view);
}

void looper_page_move(uint8_t *view, const int *pt)
{
    if (P->pressed != P_FADER)
        return;
    int x, d;
    to_page(pt, &x, &d);
    struct lay L;
    layout(P->track, &L);
    looper_set_level(P->track, (float)(L.bars_y + L.bars_h - d) / (float)L.bars_h);
    dirty(view);
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

/* Mixer view vtable +0x34 (0x080f0f44): its messages. On the page the four knobs turn the tracks' levels, or the pans
 * (the PAN button, or INFO held). */
void looper_view_msg(uint8_t *view, const uint16_t *msg)
{
    if (msg && msg[0] == MSG_KNOB && solo_looper_view() == view) {
        int knob = *(const int16_t *)((const uint8_t *)msg + 0xc);
        int counts = *(const int16_t *)((const uint8_t *)msg + 0x10);
        if (knob >= 0 && knob < LOOPER_TRACKS) {
            struct looper_info k;
            looper_track(knob, &k);
            if (((P->pan_sel >> knob) & 1) || P->info_on) {
                looper_set_pan(knob, k.pan + (float)counts * KNOB_SCALE * 2.f);
                if (P->info_on)
                    info_touch();
            } else {
                looper_set_level(knob, k.level + (float)counts * KNOB_SCALE);
            }
            dirty(view);
        }
        return;
    }
    fw_view_msg(view, msg);
}

/* INFO: tap = on until pressed again (or 10 s idle); held 0.45 s or more = on only while held. Returns 1 when the
 * message was INFO's and the stock handler must not see it. */
static int info_button(unsigned id, unsigned idx)
{
    if (id == MSG_BUTTON) {
        if (idx == BTN_MIX || idx > 7)
            return 0;
        if (P->info_idx == 0xff)
            P->info_idx = (uint8_t)idx;                           /* the first other button is taken as INFO */
        if (idx != P->info_idx)
            return 0;
        P->info_on = !P->info_on;
        P->info_down = looper_ticks();
        P->info_t = P->info_down;
        P->sig = 0;
        return 1;
    }
    if (id == MSG_RELEASE && idx == P->info_idx) {
        if (P->info_on && looper_ticks() - P->info_down >= INFO_HOLD)
            P->info_on = 0;
        P->sig = 0;
    }
    return 0;
}

/* Replaces the app's message dispatch call (bl @0x080a23cc). */
void looper_app_msg(void *app, const uint16_t *msg)
{
    if (msg && (msg[0] == MSG_BUTTON || msg[0] == MSG_RELEASE) && solo_looper_view()) {
        unsigned idx = *(const uint32_t *)((const uint8_t *)msg + 0xc);
        P->btn_id = msg[0];
        P->btn_index = (uint16_t)idx;
        if (info_button(msg[0], idx))
            return;
    }
    fw_app_msg(app, msg);
}
