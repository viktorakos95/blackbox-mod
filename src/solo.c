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
 * Looper mode is the live looper's page (looper_page.c draws it and handles its touches and knobs; the engine is
 * looper.c). This file only routes the mixer view's hooks to it while the mode shows.
 *
 * Linked into the free space after the stock image; every firmware call goes through an
 * absolute function pointer, so the object has no relocations against the stock code.
 */
#include <stdint.h>

#include "looper.h"

int looper_page_draw(uint8_t *view, uint8_t *cell, uint8_t *ctx);
void looper_page_poke(uint8_t *view);
void looper_page_enter(uint8_t *view);
void looper_page_leave(uint8_t *view);
void looper_page_down(uint8_t *view, const int *pt);
void looper_page_move(uint8_t *view, const int *pt);
void looper_page_up(uint8_t *view, const int *pt);

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
#define fw_touch_up    ((touch_fn)FN(0x080b5aec))
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

/* --- Looper mode: the page itself is looper_page.c; this routes the mixer view's hooks to it. */

#define BLK2        0x424c4b32u                            /* the page is up, not full screen */
#define APP_PTR     (*(volatile uint32_t *)0x2405ff54u)    /* the app object, as the message hook last saw it, xor'd (patch RAM is not cleared) */
#define APP_XOR     0x5a5a5a5au

/* The app's current screen id, from the app pointer the message hook saw; 0 until it has been seen. */
static unsigned app_screen(void)
{
    uint32_t p = APP_PTR ^ APP_XOR;
    if (p < 0x24000000u || p >= 0x24100000u || (p & 3))
        return 0;
    return *(volatile uint8_t *)(p + 0x8ca4u);
}

void looper_note_app(void *app)
{
    APP_PTR = (uint32_t)app ^ APP_XOR;
}


/* The mixer view while it shows the Looper page, else 0. (Step 3 also required the view to sit in 0x24000000..
 * 0x24080000, an unchecked guess: the page never drew on hardware. The view pointer is only ever the one the GUI
 * handed solo_set_mode, and looper_boot clears the mode at every boot, so a pointer from an earlier boot is not
 * used either.) */
uint8_t *solo_looper_view(void)
{
    ensure();
    uint8_t *view = S->view;
    if (!S->looper || !view || ((uint32_t)view & 3) || !view[VIEW_MUTE])
        return 0;
    unsigned sc = app_screen();                      /* another screen is up: the Looper page is not showing, whatever the flag says */
    if (sc && sc != SCREEN_MUTE)
        return 0;
    return view;
}

/* From looper_boot, once per boot: no Looper page until the GUI shows the mixer again. */
void solo_boot_reset(void)
{
    ensure();
    S->looper = 0;
    S->looper_pending = 0;
}

/* Mixer cell vtable draw (0x080effc8): the Looper page draws its own cells, everything else is stock. */
void looper_cell_draw(uint8_t *cell, uint8_t *ctx)
{
    uint8_t *view = solo_looper_view();
    if (view && ctx[0] && cell_index(view, cell) >= 0 && looper_page_draw(view, cell, ctx))
        return;
    if (view)
        return;                                    /* a cell of any other view, while the page shows: not drawn */
    fw_cell_draw(cell, ctx);
}

/* From the audio task, about 19 times a second: the page asks for a redraw when something it shows has changed. */
void looper_ui_poke(void)
{
    if (S->magic != MAGIC)
        return;
    uint8_t *view = solo_looper_view();
    if (view)
        looper_page_poke(view);
}

/* Mixer view vtable +0x18 (0x080f0f28): touch up. */
void solo_touch_up(uint8_t *view, void *pt, void *arg)
{
    ensure();
    if (S->looper && view[VIEW_MUTE]) {
        looper_page_up(view, (const int *)pt);
        return;
    }
    fw_touch_up(view, pt, arg);
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
    int was_looper = S->looper;
    S->looper = mute_mode && S->looper_pending;
    S->pending = 0;
    S->looper_pending = 0;
    if (S->looper)
        looper_page_enter(view);
    else if (was_looper)
        looper_page_leave(view);
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
        looper_page_down(view, (const int *)pt);
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
        looper_page_move(view, (const int *)pt);
        return;
    }
    if (S->active && view[VIEW_MUTE])
        return;
    fw_touch_move(view, pt, arg);
}

/* --- Looper page: keep the stock screen out of its way */

#define GUARD_BLOCK (*(volatile uint32_t *)0x2405ff58u)    /* patch RAM: exactly BLK1 = the page owns the screen */
#define GUARD_DRAW  (*(volatile uint32_t *)0x2405ff5cu)    /* exactly DRW1 = the page itself is drawing */
#define BLK1        0x424c4b31u
#define DRW1        0x44525731u
void looper_guard(int mode)                                /* 0 off, 1 page up on full screen, 2 page up */
{
    GUARD_BLOCK = mode == 1 ? BLK1 : mode == 2 ? BLK2 : 0;
}

void looper_guard_drawing(int on)
{
    GUARD_DRAW = on ? DRW1 : 0;
}

/* The page owns the screen right now (it is showing, full screen, on the mixer screen). */
static int page_owns_screen(void)
{
    return GUARD_BLOCK == BLK1 && S->magic == MAGIC && S->looper && S->view && app_screen() == SCREEN_MUTE;
}

/* From the drawing primitive stubs (looper_thunk.S): 1 = drop this stock draw. Must stay tiny: it runs per pixel. */
int looper_draw_blocked(void)
{
    return GUARD_DRAW != DRW1 && page_owns_screen();
}

typedef int (*whit_fn)(uint8_t *w, const int *pt, uint8_t **out);
typedef int (*rect_fn)(uint8_t *w, const int *pt);
typedef int (*chk_fn)(uint8_t *w);

/*
 * Replaces the base widget's touch hit test (FUN_080aeb5e, patched in at its entry; every widget class shares it):
 * the widget at the point, children first. The stock one is reproduced here exactly. While the Looper page owns the
 * screen the answer is always the mixer view, so no widget of the stock screen (the cyan boxes, the top bar) can take a
 * touch that is meant for the page, whatever part of the screen it is on.
 */
int looper_basehit(uint8_t *w, const int *pt, uint8_t **out)
{
    if (page_owns_screen() && S->view[VIEW_MUTE]) {
        *out = S->view;
        return 1;
    }
    if (w[0x30])
        return 0;
    for (uint8_t *c = *(uint8_t **)(w + 0x18); c; c = *(uint8_t **)(c + 0x20)) {
        int r = (*(whit_fn *)(*(uint8_t **)c + 0x28))(c, pt, out);
        if (r)
            return r;
    }
    int r = (*(rect_fn *)(*(uint8_t **)w + 0xc))(w, pt);
    if (!r)
        return 0;
    r = (*(chk_fn *)(*(uint8_t **)w + 0x1c))(w);
    if (!r)
        return 0;
    *out = w;
    return r;
}

#define fw_text_draw ((draw_fn)FN(0x080c3d2c))

/* The Looper page is showing (not just flagged): the mixer screen, mute mode, Looper mode. */
static int page_visible(void)
{
    return (GUARD_BLOCK == BLK1 || GUARD_BLOCK == BLK2) && S->magic == MAGIC && S->looper && S->view && app_screen() == SCREEN_MUTE &&
           S->view[VIEW_MUTE];
}

/*
 * Draw slot of the text widget class (vtable 0x080f17a4 + 4, stock 0x080c3d2c). The cyan boxes that stayed over the page
 * are text widgets inside the mixer cells (three per cell, cell + 0x70 / 0xc8 / 0x120), drawn whatever their hidden flag
 * says; and the screen's own top bar is text too. While the page shows, a text widget inside the mixer view is not drawn;
 * on full screen no text widget is.
 */
void looper_text_draw(uint8_t *w, void *ctx)
{
    if (page_visible() && (GUARD_BLOCK == BLK1 || (uint32_t)(w - S->view) < 0x2000u))
        return;
    fw_text_draw(w, ctx);
}

/* From the line / text stubs: 1 = drop this stock draw (the page is showing and is not the one drawing). */
void looper_page_dropped(void);
int looper_page_swallow(uint32_t w0);

int looper_stock_blocked(void)
{
    if (GUARD_DRAW != DRW1 && page_visible()) {
        looper_page_dropped();
        return 1;
    }
    return 0;
}

/*
 * From the rectangle fill stub (looper_thunk.S, hook at 0x0808f920; r0 fb, r1 colour, r2 -> {u16 0, 0, w, h, x, y}-style
 * block: w +4, h +6, x +8, y +0xa): stock fills in the side margins (x < 32 or x >= 288, outside the mixer cells) are
 * dropped while the page shows: the widgets there redraw after the page and leave bits of themselves on its left side.
 * Only these narrow fills: dropping every fill stopped the display from updating (steps 12-14).
 */
int looper_fill_blocked(void *fb, int color, const uint16_t *r)
{
    (void)fb;
    (void)color;
    if (GUARD_DRAW == DRW1 || !page_visible())
        return 0;
    int x = r[4], w = r[2];
    if (w > 0 && (x + w <= 32 || x >= 288)) {
        looper_page_dropped();
        return 1;
    }
    return 0;
}

void looper_page_event(uint32_t w0, uint32_t w1);
void looper_page_dropped(void);

/* Diagnostic, from the engine event post stub: remember what is queued while the page shows. */
int looper_note_event(void *list, const uint32_t *ev)
{
    (void)list;
    if (!page_visible())
        return 0;
    looper_page_event(ev[0], ev[1]);
    /* The transport buttons' events (seen on the unit as 0x49, 0x4f and 0x70 for PLAY, STOP and REC in some order):
     * dropped while the page shows, so the buttons are the looper's alone; HW STOP PLAY = +STOCK lets them through. */
    uint32_t id = ev[0] & 0xffffffu;
    if (looper_page_swallow(ev[0])) {
        looper_page_dropped();
        return 1;
    }
    if (0 && (id == 0x49 || id == 0x4f || id == 0x70) && looper_get_opt(LOOPER_O_HWBTN) < .5f) {
        looper_page_dropped();
        return 1;
    }
    return 0;
}

/* The key scanner posts every key event twice: to the GUI queue (looper_app_msg sees it) and to a second ring the audio
 * task pops with FUN_08043a2c (callers 0x0804ccc8 / 0x0804ce30), which is where PLAY / STOP / REC reach the sequencer.
 * Wrapped here: while the page shows, those three keys (index 8 REC, 9 STOP, 10 PLAY) are dropped from that ring
 * (STOP / PLAY only with HW STOP PLAY = LOOPER). The GUI queue still delivers them to the page. */
typedef int (*keypop_fn)(void *obj, uint8_t *ev);
int looper_key_pop(void *obj, uint8_t *ev)
{
    for (;;) {
        int r = ((keypop_fn)0x08043a2du)(obj, ev);
        if (!r || !page_visible())
            return r;
        unsigned id = *(const uint16_t *)ev, idx = *(const uint32_t *)(ev + 0xc);
        if (id < 0xf4 || id > 0xf8 || idx < 8 || idx > 10)
            return r;
        if (idx != 8 && looper_get_opt(LOOPER_O_HWBTN) > .5f)
            return r;
        looper_page_dropped();
    }
}

unsigned looper_screen_id(void)
{
    return app_screen();
}
