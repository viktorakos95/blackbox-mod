/*
 * Mixer Solo for the original 1010music Blackbox, firmware 3.1.9.
 *
 * MIX cycles Mixer (screen 0x2e) -> Mute (0x2f) -> Solo (0x2f + solo flag) -> Mixer.
 * In Solo mode a tap toggles a pad in the solo set; while the set is non-empty every other
 * pad is muted through the firmware's own mute path (GUI msg 0x44 -> engine param 100).
 * Emptying the set restores the mutes captured when the first pad was soloed.
 * If mutes are edited by hand (a tap in plain Mute mode) while a solo set exists, the set is dropped: the hand edit
 * wins and nothing is restored later. Nothing else drops it (3.1.l and earlier also dropped it whenever a soloed pad
 * merely READ muted at the next tap, then re-captured the solo's own mutes as the state to return to: un-solo left
 * everything muted. His report on 3.1.l.)
 *
 * Linked into the free space after the stock image; every firmware call goes through an
 * absolute function pointer, so the object has no relocations against the stock code.
 */
#include <stdint.h>

#define FN(addr) ((addr) | 1u)

typedef void (*fill_fn)(void *rect, int color, void *fb);
typedef void (*post_fn)(void *view, void *msg);
typedef void (*cell_set_fn)(void *cell, int unmuted);
typedef int (*hit_fn)(void *view, void *pt, uint8_t **cell);
typedef void (*touch_fn)(void *view, void *pt, void *arg);
typedef void (*set_mode_fn)(void *view, int mute_mode);
typedef void (*set_screen_fn)(void *app, int screen, int a, int b);

#define fw_fill        ((fill_fn)FN(0x0808ea22))
#define fw_outline     ((fill_fn)FN(0x0808e994))
#define fw_post        ((post_fn)FN(0x080aed14))
#define fw_cell_set    ((cell_set_fn)FN(0x080a4f10))
#define fw_hit         ((hit_fn)FN(0x080b5e78))
#define fw_touch_down  ((touch_fn)FN(0x080b5f44))
#define fw_touch_move  ((touch_fn)FN(0x080b5ebc))
#define fw_set_mode    ((set_mode_fn)FN(0x080b5e44))
#define fw_set_screen  ((set_screen_fn)FN(0x0809eaec))

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
    uint8_t pad[2];
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

/* Replaces the set-screen call in the MIX button handler. */
void solo_mix_pressed(uint8_t *app, int computed, int a, int b)
{
    (void)computed;
    ensure();
    int cur = app[APP_SCREEN], next = SCREEN_MIXER;
    if (cur == SCREEN_MIXER) {
        next = SCREEN_MUTE;
    } else if (cur == SCREEN_MUTE && !S->active) {
        S->pending = 1;
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
    S->pending = 0;
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
    if (S->active && view[VIEW_MUTE])
        return;
    fw_touch_move(view, pt, arg);
}
