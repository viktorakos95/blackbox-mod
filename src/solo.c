/*
 * Mixer Solo for the original 1010music Blackbox, firmware 3.1.9.
 *
 * MIX cycles Mixer (screen 0x2e) -> Mute (0x2f) -> Solo (0x2f + solo flag) -> Looper (0x2f + looper flag) -> Mixer.
 * In Solo mode a tap toggles a pad in the solo set; while the set is non-empty every other
 * pad is muted through the firmware's own mute path (GUI msg 0x44 -> engine param 100).
 * Emptying the set restores the mutes captured when the first pad was soloed.
 * If mutes are edited by hand (a tap in plain Mute mode) while a solo set exists, the set is dropped: the hand edit
 * wins and nothing is restored later. Nothing else drops it (3.1.l and earlier also dropped it whenever a soloed pad
 * merely READ muted at the next tap, then re-captured the solo's own mutes as the state to return to: un-solo left
 * everything muted. His report on 3.1.l.)
 *
 * Looper mode (the live looper, engine in looper.c) turns the 16 mixer cells into its controls, one column per track:
 *   row 0  REC / PLAY / DUB: dark = empty, red = recording, yellow = overdubbing, green = playing, white = clearing.
 *          A cyan line along its bottom is the loop's playhead (or, while the first take records, how much of the
 *          20 s it has used).
 *   row 1  MUTE: green = playing, red = muted, dark = empty track.
 *   row 2  CLEAR: tap to wipe the track.
 *   row 3  LEVEL: a fader; touch it and drag up / down. The cyan bar is the level.
 * The stock cell drawing is skipped for these cells while Looper mode shows (mixer cell vtable +0x20 draw), so pad
 * names do not show there. The GUI redraws the cells when the looper marks them dirty (from the audio task, about
 * 20 times a second while the mode shows: a plain byte write, as the CPU meter does for its label).
 *
 * Linked into the free space after the stock image; every firmware call goes through an
 * absolute function pointer, so the object has no relocations against the stock code.
 */
#include <stdint.h>

#include "looper.h"

#define FN(addr) ((addr) | 1u)

typedef void (*fill_fn)(void *rect, int color, void *fb);
typedef void (*post_fn)(void *view, void *msg);
typedef void (*cell_set_fn)(void *cell, int unmuted);
typedef int (*hit_fn)(void *view, void *pt, uint8_t **cell);
typedef void (*touch_fn)(void *view, void *pt, void *arg);
typedef void (*set_mode_fn)(void *view, int mute_mode);
typedef void (*set_screen_fn)(void *app, int screen, int a, int b);
typedef void (*draw_fn)(uint8_t *cell, uint8_t *ctx);

#define fw_fill        ((fill_fn)FN(0x0808ea22))
#define fw_outline     ((fill_fn)FN(0x0808e994))
#define fw_post        ((post_fn)FN(0x080aed14))
#define fw_cell_set    ((cell_set_fn)FN(0x080a4f10))
#define fw_hit         ((hit_fn)FN(0x080b5e78))
#define fw_touch_down  ((touch_fn)FN(0x080b5f44))
#define fw_touch_move  ((touch_fn)FN(0x080b5ebc))
#define fw_set_mode    ((set_mode_fn)FN(0x080b5e44))
#define fw_set_screen  ((set_screen_fn)FN(0x0809eaec))
#define fw_cell_draw   ((draw_fn)FN(0x080a43b8))

#define SCREEN_MIXER 0x2e
#define SCREEN_MUTE  0x2f
#define APP_SCREEN   0x8ca4      /* app: current screen id */
#define VIEW_MUTE    0x1e40      /* mixer view: mute-mode flag */
#define VIEW_CELLS   0x3ac       /* mixer view: first of 16 cells */
#define CELL_SIZE    0x1a0
#define CELL_PAD     0x38        /* u16 pad id: bits 0-3 col, 4-7 row */
#define CELL_DIRTY   0x17c
#define CELL_UNMUTED 0x189
#define COLOR_SOLO_FILL 0x1c     /* palette: rgb(119,113,0) */
#define COLOR_SOLO_EDGE 0x14     /* palette: rgb(255,255,0) */
#define CELL_RECT    0x04        /* int x, y, w, h */
#define CTX_FB       0x04        /* draw context: frame buffer handed to fill / outline */
#define COLOR_DARK   0x19        /* palette: rgb(43,43,43) */
#define COLOR_GREY   0x09        /* rgb(102,102,102) */
#define COLOR_MID    0x10        /* rgb(68,68,68) */
#define COLOR_LIGHT  0x16        /* rgb(170,170,170) */
#define COLOR_WHITE  0x0f
#define COLOR_GREEN  0x0b        /* rgb(34,187,34): the stock "unmuted" */
#define COLOR_RED    0x0c        /* rgb(187,34,34): the stock "muted" */
#define COLOR_REC    0x06        /* rgb(255,0,0) */
#define COLOR_YELLOW 0x14        /* rgb(255,255,0) */
#define COLOR_CYAN   0x1b        /* rgb(9,215,245) */
#define COLOR_TEAL   0x1a        /* rgb(4,107,122) */
#define MSG_PAD_MUTE 0x44
#define MSG_TAG      0x080cfba4u

/* Patch RAM: the 172-byte alignment gap between the heap end (0x2405ff54) and the MPU regions at 0x24060000.
 * Never take it from the heap: operator new allocates 167,704 bytes of that 200,000-byte heap in one block and
 * hangs on failure (build 3.1.U crash-looped that way). Not zeroed at boot. */
struct solo_state {
    uint32_t magic;
    uint8_t active;              /* mixer view is showing Solo mode */
    uint8_t pending;             /* MIX asked for Solo; consumed by the next mode change */
    uint8_t looper;              /* mixer view is showing Looper mode */
    uint8_t looper_pending;      /* MIX asked for Looper; consumed by the next mode change */
    uint8_t *view;
    uint16_t mask;               /* soloed cells, bit = cell index */
    uint16_t saved;              /* muted cells captured when the set became non-empty */
};
#define S ((volatile struct solo_state *)0x2405ff60u)
#define MAGIC 0x534f4c4fu        /* "SOLO" */

struct pad_msg {
    uint16_t id, _r0;
    uint32_t tag;
    uint16_t pad, _r1;
    uint32_t unmuted;
    uint32_t a, b;
};

static void ensure(void)
{
    if (S->magic != MAGIC) {
        S->active = 0;
        S->pending = 0;
        S->looper = 0;
        S->looper_pending = 0;
        S->view = 0;
        S->mask = 0;
        S->saved = 0;
        S->magic = MAGIC;
    }
}

static uint8_t *cell_at(uint8_t *view, int i)
{
    return view + VIEW_CELLS + i * CELL_SIZE;
}

static int pad_ok(uint8_t *cell)
{
    return (*(uint16_t *)(cell + CELL_PAD) & 0xf) <= 3;
}

static int cell_index(uint8_t *view, uint8_t *cell)
{
    if (!view || cell < cell_at(view, 0) || cell > cell_at(view, 15))
        return -1;
    int off = cell - cell_at(view, 0);
    return off % CELL_SIZE ? -1 : off / CELL_SIZE;
}

/*
 * The solo set counts while every soloed pad is still unmuted. (3.1.d and earlier also required every other pad to be
 * muted; a pad that does not hold a mute, such as an empty one, then broke the set, and un-soloing re-captured
 * "everything muted" as the state to return to: nothing played after un-solo.)
 */
static int solo_valid(uint8_t *view)
{
    uint16_t mask = S->mask;
    if (!mask)
        return 0;
    for (int i = 0; i < 16; i++) {
        uint8_t *c = cell_at(view, i);
        if (pad_ok(c) && ((mask >> i) & 1) && !c[CELL_UNMUTED])
            return 0;
    }
    return 1;
}

static void mark_dirty(uint8_t *view)
{
    for (int i = 0; i < 16; i++)
        cell_at(view, i)[CELL_DIRTY] = 1;
}

static void apply(uint8_t *view)
{
    uint16_t mask = S->mask, saved = S->saved;
    for (int i = 0; i < 16; i++) {
        uint8_t *c = cell_at(view, i);
        if (!pad_ok(c))
            continue;
        int want = mask ? (mask >> i) & 1 : !((saved >> i) & 1);
        if (c[CELL_UNMUTED] == want)
            continue;
        struct pad_msg m = {MSG_PAD_MUTE, 0, MSG_TAG, *(uint16_t *)(c + CELL_PAD), 0, (uint32_t)want, 0, 0};
        fw_post(view, &m);
        fw_cell_set(c, want);
    }
    mark_dirty(view);
}

static void toggle(uint8_t *view, uint8_t *cell)
{
    int i = cell_index(view, cell);
    if (i < 0 || !pad_ok(cell))
        return;
    if (!S->mask) {
        uint16_t saved = 0;
        for (int j = 0; j < 16; j++)
            if (!cell_at(view, j)[CELL_UNMUTED])
                saved |= 1u << j;
        S->saved = saved;
    }
    S->mask ^= 1u << i;
    apply(view);
}

/* --- Looper mode */

/* Fader drag, in the looper's backup SRAM page (after its engine state). */
struct looper_drag {
    int32_t track;               /* -1 = no drag */
    int32_t y0;
    float level0, span;          /* level at touch down, pixels for the full range */
};
#define DRAG ((volatile struct looper_drag *)0x38800f00u)

static int cell_row(uint8_t *cell)
{
    return (*(uint16_t *)(cell + CELL_PAD) >> 4) & 0xf;
}

static int cell_col(uint8_t *cell)
{
    return *(uint16_t *)(cell + CELL_PAD) & 0xf;
}

static void looper_touch(uint8_t *view, const int *pt)
{
    uint8_t *cell = 0;
    DRAG->track = -1;
    if (!fw_hit(view, (void *)pt, &cell) || !cell || cell_index(view, cell) < 0)
        return;
    int t = cell_col(cell), row = cell_row(cell);
    if (t >= LOOPER_TRACKS)
        return;
    if (row == 0) {
        looper_command(t, LOOPER_CMD_REC);
    } else if (row == 1) {
        looper_command(t, LOOPER_CMD_MUTE);
    } else if (row == 2) {
        looper_command(t, LOOPER_CMD_CLEAR);
    } else if (row == 3) {
        struct looper_info k;
        looper_track(t, &k);
        int h = ((int *)(cell + CELL_RECT))[3];
        DRAG->level0 = k.level;
        DRAG->y0 = pt[1];
        DRAG->span = (float)(h > 8 ? 2 * h : 100);
        DRAG->track = t;
    }
    mark_dirty(view);
}

static void looper_drag(uint8_t *view, const int *pt)
{
    int t = DRAG->track;
    if (t < 0 || t >= LOOPER_TRACKS)
        return;
    looper_set_level(t, DRAG->level0 + (float)(DRAG->y0 - pt[1]) / DRAG->span);
    mark_dirty(view);
}

static void fill(const int *r, int x, int y, int w, int h, int color, void *fb)
{
    int q[4] = {r[0] + x, r[1] + y, w, h};
    if (w > 0 && h > 0)
        fw_fill(q, color, fb);
}

static void looper_draw(uint8_t *cell, uint8_t *ctx)
{
    const int *r = (const int *)(cell + CELL_RECT);
    void *fb = *(void **)(ctx + CTX_FB);
    int t = cell_col(cell), row = cell_row(cell), w = r[2], h = r[3];
    struct looper_info k;
    looper_track(t, &k);
    int has = k.mode == LOOPER_PLAY || k.mode == LOOPER_DUB;
    int color = COLOR_DARK;
    if (row == 0) {
        color = k.mode == LOOPER_REC ? COLOR_REC : k.mode == LOOPER_DUB ? COLOR_YELLOW
              : k.mode == LOOPER_PLAY ? COLOR_GREEN : k.mode == LOOPER_CLEARING ? COLOR_WHITE : COLOR_DARK;
    } else if (row == 1) {
        color = !has ? COLOR_DARK : k.muted ? COLOR_RED : COLOR_GREEN;
    } else if (row == 2) {
        color = k.mode == LOOPER_CLEARING ? COLOR_WHITE : COLOR_MID;
    }
    fill(r, 0, 0, w, h, color, fb);
    if (row == 0 && (has || k.mode == LOOPER_REC)) {
        float p = looper_progress();
        fill(r, 0, h - 5, (int)(p * (float)w), 5, COLOR_CYAN, fb);
    } else if (row == 2) {
        fill(r, w / 4, h / 2 - 2, w / 2, 4, COLOR_LIGHT, fb);     /* a dash: "wipe" */
    } else if (row == 3) {
        int bar = (int)(k.level * (float)(h - 4));
        fill(r, 4, h - 2 - bar, w - 8, bar, k.muted ? COLOR_TEAL : COLOR_CYAN, fb);
    }
    fw_outline((void *)r, COLOR_GREY, fb);
}

/* Mixer cell vtable draw (0x080effc8): Looper mode draws its own cells, everything else is stock. */
void looper_cell_draw(uint8_t *cell, uint8_t *ctx)
{
    ensure();
    if (S->looper && S->view && ctx[0] && cell_index(S->view, cell) >= 0 && cell_col(cell) < LOOPER_TRACKS &&
        cell_row(cell) < 4) {
        looper_draw(cell, ctx);
        return;
    }
    fw_cell_draw(cell, ctx);
}

/* From the audio task, about 20 times a second: have the GUI redraw the looper cells (playhead, state changes). */
void looper_ui_poke(void)
{
    if (S->magic != MAGIC || !S->looper)
        return;
    uint8_t *view = S->view;
    if ((uint32_t)view < 0x24000000u || (uint32_t)view >= 0x24080000u || !view[VIEW_MUTE])
        return;
    mark_dirty(view);
}

/* Replaces the set-screen call in the MIX button handler. */
void solo_mix_pressed(uint8_t *app, int computed, int a, int b)
{
    (void)computed;
    ensure();
    int cur = app[APP_SCREEN], next = SCREEN_MIXER;
    if (cur == SCREEN_MIXER) {
        next = SCREEN_MUTE;
    } else if (cur == SCREEN_MUTE && !S->active && !S->looper) {
        S->pending = 1;
        next = SCREEN_MUTE;
    } else if (cur == SCREEN_MUTE && S->active && looper_ready()) {
        S->looper_pending = 1;
        next = SCREEN_MUTE;
    }
    fw_set_screen(app, next, a, b);
}

/* Replaces the GUI's call to the mixer view's set-mode on every mixer (re)show. */
void solo_set_mode(uint8_t *view, int mute_mode)
{
    ensure();
    S->view = view;
    S->active = mute_mode && S->pending;
    S->looper = mute_mode && S->looper_pending;
    S->pending = 0;
    S->looper_pending = 0;
    fw_set_mode(view, mute_mode);
    mark_dirty(view);
}

/* Replaces the green/red fill in the mixer cell's mute-mode draw. rect = cell + 4. */
void solo_mute_fill(uint8_t *rect, int color, void *fb)
{
    ensure();
    uint8_t *view = S->view;
    int i = S->active ? cell_index(view, rect - 4) : -1;
    if (i >= 0 && solo_valid(view) && ((S->mask >> i) & 1))
        color = COLOR_SOLO_FILL;
    fw_fill(rect, color, fb);
    if (i >= 0) {
        int *r = (int *)rect;
        int inner[4] = {r[0] + 1, r[1] + 1, r[2] - 2, r[3] - 2};
        fw_outline(rect, COLOR_SOLO_EDGE, fb);
        fw_outline(inner, COLOR_SOLO_EDGE, fb);
    }
}

void solo_touch_down(uint8_t *view, void *pt, void *arg)
{
    ensure();
    if (S->looper && view[VIEW_MUTE]) {
        looper_touch(view, (const int *)pt);
        return;
    }
    if (S->active && view[VIEW_MUTE]) {
        uint8_t *cell = 0;
        if (fw_hit(view, pt, &cell) && cell)
            toggle(view, cell);
        return;
    }
    if (view[VIEW_MUTE])
        S->mask = 0;                     /* a hand edit of the mutes: the solo set is gone, keep what he set */
    fw_touch_down(view, pt, arg);
}

void solo_touch_move(uint8_t *view, void *pt, void *arg)
{
    ensure();
    if (S->looper && view[VIEW_MUTE]) {
        looper_drag(view, (const int *)pt);
        return;
    }
    if (S->active && view[VIEW_MUTE])
        return;
    fw_touch_move(view, pt, arg);
}
