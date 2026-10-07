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
 * Three tabs in the footer: MAIN (the above), FX and SETUP.
 *   FX     per track: the record box, then four dials (FILT: low pass left of centre, high pass right; CRSH: bit and
 *          rate crunch; DLY and RVB: sends into the looper's own delay and reverb), REV and HALF (half speed) buttons
 *          and MUTE. Tap a dial to select it (pink): the track's knob then turns it (and, with INFO, the level); drag
 *          up and down on a dial to turn it by touch.
 *   SETUP  the options: LENGTH of later tracks (FOLLOW the first loop / MULT: a multiple of it / FREE), SYNC to the
 *          Blackbox tempo with the QUANT grid, the recording SOURCE (input or the mix), the delay time and the four
 *          effect amounts (knobs 1-4), CLEAR ALL, and the tempo / transport the clock features see.
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
#define fw_post     ((msg_fn)FN(0x080b5758))               /* queue a message for the GUI task (app + 0x30) */

uint8_t *solo_looper_view(void);
void looper_guard(int mode);
void looper_note_app(void *app);
void looper_guard_drawing(int on);
static void rehide(uint8_t *view);
static void paint(uint8_t *view);
static void widen(uint8_t *view);

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

#define MSG_PAINT   0x1f0        /* our own: "repaint the page" (posted by the audio task's poke, handled in the GUI task) */
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
#define FOOT      14             /* the footer; its line is at d = hg - FOOT - 1 */
#define REC_H     40             /* the record box */
#define BTN       34             /* the bottom row: the pan dial and the REV / MUTE buttons */
#define DIAL      34             /* width of the pan dial's box */

#define INFO_TIMEOUT 1875        /* audio blocks (10 s) */
#define INFO_HOLD    84          /* 0.45 s */

enum { P_NONE, P_REC, P_MUTE, P_FADER, P_DIAL, P_SLIDER };
enum { Z_NONE, Z_REC, Z_FADER, Z_PAN, Z_REV, Z_MUTE, Z_TAB, Z_FX, Z_HALF, Z_OPTC, Z_SLIDER, Z_CLEAR, Z_UNDO, Z_SEL };
enum { M_MAIN, M_FX, M_SETUP, M_MORE, MODES };
/* hardware buttons the page can take over: slot numbers */
enum { B_NONE, B_FX, B_REC, B_BACK, B_STOP, B_PLAY, BTNS };

#define TAB_W      38
#define ROW_H      22                      /* SETUP: one option per row */
#define OPT_X      84                      /* SETUP: where the choices start */
#define CHOICE_W   52
#define SLIDER_W   150
#define CLEAR_WAIT 563                     /* audio blocks (3 s) to confirm CLEAR ALL */

struct page {
    int32_t _legacy;             /* step 2's fader drag; looper_boot used to write -1 here */
    uint8_t pressed, track, pan_sel, info_on;
    uint8_t info_idx, info_set, entered, hid_on;     /* info_set: the INFO button has been learned */
    uint16_t btn_id, btn_index;                      /* the last button message seen on the page */
    uint16_t info_id, _r2;                           /* the learned INFO: message id (and button, for 0xf9) */
    uint32_t magic, info_t, info_down, sig, force, fb;
    uint32_t hid[2];             /* the cells' child widgets' own hidden flags, saved while the page hides them */
    int32_t ytop, hg, s, sx;     /* geometry read from the cells at the last draw */
    int32_t x0, w;               /* the page's left edge and width, in fill space */
    uint8_t mode, fx_sel[LOOPER_TRACKS];            /* the tab; per track the selected FX dial */
    uint8_t drag_param, drag_opt;                   /* a dial / slider being dragged */
    int16_t drag_y, drag_v0;                        /* dial drag: start y, start value x 1000 */
    uint8_t clear_arm, sel, learn, _r5;            /* CLEAR ALL asked once; the selected track; the slot being learned */
    uint16_t rect_on, touches;
    uint8_t bset[BTNS], bidx[BTNS];                 /* learned hardware buttons: set, button index ... */
    uint16_t bid[BTNS];                             /* ... and message id, per slot */
    uint32_t rec_t, btn_t, paint_t, dropped;
    uint32_t ev0[4], ev1[4];                         /* the last distinct engine events queued (diagnostic, shown on MORE) */
    uint8_t paint_req, _r6[3];
    int16_t touch_x, touch_y;
    uint32_t clear_t;                               /* when CLEAR ALL was asked */
    int32_t vrect[4];                               /* the mixer view's own rectangle, while the page widens it */
};
#define P ((volatile struct page *)0x38800f00u)
#define PMAGIC 0x50414732u

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
    /* FULL SCREEN: grow the page upward over the screen's own top bar (the page keeps its bottom edge) */
    int fbh = *(const uint16_t *)((const uint8_t *)FB + 6);
    if (looper_get_opt(LOOPER_O_FULL) > .5f && fbh >= P->hg + 4 && fbh <= P->hg + 40) {
        int extra = fbh - P->hg;
        P->ytop += P->s > 0 ? extra : -extra;
        P->hg = fbh;
    }
    /* The cells are centred on the screen, so the screen is as wide as the cells plus their margin twice; the frame
     * buffer's own width (fb + 4) caps it when it makes sense. Never wider than that: a fill past the edge is unsafe. */
    int ws = x_lo + x_hi, fbw = *(const uint16_t *)((const uint8_t *)FB + 4);
    if (fbw >= 160 && fbw <= 1024 && fbw < ws)
        ws = fbw;
    if (ws < x_hi || ws > 1024 || x_hi - x_lo < 80) {              /* not centred, or nothing sensible: the cells only */
        P->x0 = x_lo;
        P->w = x_hi - x_lo;
    } else {
        int full = looper_get_opt(LOOPER_O_FULL) > .5f;
        P->x0 = full ? 0 : MARGIN;                                /* full screen: right to the edges */
        P->w = full ? ws : ws - 2 * MARGIN;
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

/* The fader: the bottom is silence, 3/4 of the way up is unity, the top is +6 dB (gain 2): like a mixer. */
static float gain_of(float pos)
{
    pos = pos < 0.f ? 0.f : pos > 1.f ? 1.f : pos;
    return pos <= 0.75f ? pos * (1.f / 0.75f) : 1.f + (pos - 0.75f) * 4.f;
}

static float pos_of(float gain)
{
    return gain <= 1.f ? gain * 0.75f : 0.75f + (gain - 1.f) * 0.25f;
}

struct lay {
    int cx, cw;                  /* the column (x from the page's left edge) */
    int col_y, col_h;
    int rec_y, rec_h;            /* the record box */
    int bars_y, bars_h;          /* the fader's travel */
    int lvl_y, btn_y, undo_y, sel_y;
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
    L->rec_h = P->mode == M_FX ? 22 : REC_H;
    L->btn_y = L->col_y + L->col_h - 2 - BTN;
    L->undo_y = L->btn_y - 15;
    L->lvl_y = L->undo_y - 12;
    L->sel_y = L->rec_y + L->rec_h + 3;
    L->bars_y = L->sel_y + 16;
    L->bars_h = L->lvl_y - 4 - L->bars_y;
    if (L->bars_h < 20)
        L->bars_h = 20;
    L->foot_y = hg - FOOT - 1;
}

/* SETUP / MORE rows: a name, the option, and either its choices or (n = 0) a slider; a last row of buttons follows. */
struct optrow {
    const char *name;
    uint8_t opt, n;
    const char *c[4];
};
static const struct optrow rows_setup[] = {
    {"LENGTH", LOOPER_O_LEN, 3, {"FOLLOW", "MULT", "FREE", 0}},
    {"SYNC", LOOPER_O_SYNC, 2, {"OFF", "ON", 0, 0}},
    {"QUANT", LOOPER_O_QUANT, 3, {"1/4", "1/8", "1/16", 0}},
    {"SOURCE", LOOPER_O_SRC, 2, {"INPUT", "MIX", 0, 0}},
    {"FULL SCREEN", LOOPER_O_FULL, 2, {"OFF", "ON", 0, 0}},
    {"LOOP GAIN K1", LOOPER_O_GAIN, 0, {0, 0, 0, 0}},
    {"HW STOP PLAY", LOOPER_O_HWBTN, 2, {"LOOPER", "+STOCK", 0, 0}},
};
static const struct optrow rows_more[] = {
    {"FX ROUTE", LOOPER_O_ROUTE, 2, {"STOCK", "OWN", 0, 0}},
    {"OWN DLY TIME", LOOPER_O_DTIME, 4, {"1/8", "1/4", "D.1/8", "D.1/4"}},
    {"OWN DLY FB K1", LOOPER_O_DFB, 0, {0, 0, 0, 0}},
    {"OWN DLY RET K2", LOOPER_O_DRET, 0, {0, 0, 0, 0}},
    {"OWN RVB SIZE K3", LOOPER_O_RSIZE, 0, {0, 0, 0, 0}},
    {"OWN RVB RET K4", LOOPER_O_RRET, 0, {0, 0, 0, 0}},
};
#define OPT_ROWS opt_rows()                                       /* rows above the buttons row: 7 on SETUP, 6 on MORE */
static int opt_rows(void);

static int opt_rows(void)
{
    return P->mode == M_MORE ? 6 : 7;
}

static const struct optrow *page_rows(void)
{
    return P->mode == M_MORE ? rows_more : rows_setup;
}

static int row_d(const struct lay *L, int r)
{
    return L->col_y + 3 + r * ROW_H;
}

/* FX tab: where the dial rows start (below the record box) */
static int fx_top(const struct lay *L)
{
    return L->sel_y + 16;                                         /* below the record box and its SELECT button */
}

/* FX tab: height of the two button rows (REV | HALF, MUTE | UNDO): they share what the dials leave. */
static int fx_btn_h(const struct lay *L)
{
    int y1 = fx_top(L) + 3 * (BTN + 1), h = (L->col_y + L->col_h - y1 - 3) / 2;
    return h < 14 ? 14 : h > 34 ? 34 : h;
}

/* Which control is at x pixels from the left and d from the top. For a fader *val is its gain there; for a tab
 * *track is the tab, for an FX dial *val its parameter, for a SETUP choice *track is the option and *val the choice,
 * for a slider *val is 0..1. */
int looper_page_hit(int x, int d, int *track, float *val)
{
    struct lay L;
    int cw = (P->w - 2 - 3 * GAP) / 4;
    if (P->hg < 120 || P->w < 80 || cw <= 0)
        return Z_NONE;                                            /* (hg is 0 until the page has been drawn once) */
    layout(0, &L);
    if (d > L.foot_y) {                                           /* the footer: the tabs */
        int i = (x - 3) / (TAB_W + 2);
        if (x >= 3 && i < MODES && (x - 3) % (TAB_W + 2) < TAB_W) {
            *track = i;
            return Z_TAB;
        }
        return Z_NONE;
    }
    if (P->mode == M_SETUP || P->mode == M_MORE) {
        if (d < L.col_y)
            return Z_NONE;
        int r = (d - row_d(&L, 0)) / ROW_H;
        if (d < row_d(&L, 0) || r > OPT_ROWS + 1 || (d - row_d(&L, 0)) % ROW_H >= ROW_H - 3)
            return Z_NONE;
        if (P->mode == M_MORE && r >= OPT_ROWS) {                 /* two rows of three buttons: FX REC BACK / STOP PLAY STOCK */
            int b = (x - 6) / 102;
            if (x < 6 || b > 2 || (x - 6) % 102 >= 96)
                return Z_NONE;
            *track = 100 - ((r - OPT_ROWS) * 3 + b + 1);          /* 99 FX, 98 REC, 97 BACK, 96 STOP, 95 PLAY, 94 STOCK FX */
            return Z_OPTC;
        }
        if (r == OPT_ROWS) {
            if (P->mode == M_SETUP)
                return x >= OPT_X && x < OPT_X + 110 ? Z_CLEAR : Z_NONE;
            return Z_NONE;
        }
        if (r > OPT_ROWS)
            return Z_NONE;
        if (0) {
            return Z_NONE;
        }
        const struct optrow *R = &page_rows()[r];
        *track = R->opt;
        if (R->n) {
            int c = (x - OPT_X) / CHOICE_W;
            if (x < OPT_X || c >= R->n)
                return Z_NONE;
            *val = (float)c;
            return Z_OPTC;
        }
        if (x < OPT_X - 6 || x > OPT_X + SLIDER_W + 6)
            return Z_NONE;
        float v = (float)(x - OPT_X) / (float)SLIDER_W;
        *val = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
        return Z_SLIDER;
    }
    int t = x >= 1 ? (x - 1) / (cw + GAP) : -1;
    if (t < 0 || t >= LOOPER_TRACKS || x - 1 - t * (cw + GAP) >= cw)
        return Z_NONE;
    layout(t, &L);
    *track = t;
    if (d >= L.rec_y - 2 && d < L.rec_y + L.rec_h + 2)
        return Z_REC;
    if (P->mode == M_FX) {
        int y0 = fx_top(&L), xx = x - L.cx - 2;
        if (d >= L.sel_y && d < L.sel_y + 13)
            return Z_SEL;
        if (d >= y0 && d < y0 + 3 * (BTN + 1) && xx >= 0 && xx < 2 * (DIAL + 1) && (d - y0) % (BTN + 1) < BTN &&
            xx % (DIAL + 1) < DIAL) {
            *val = (float)(((d - y0) / (BTN + 1)) * 2 + xx / (DIAL + 1));
            return Z_FX;
        }
        int y1 = y0 + 3 * (BTN + 1), hb = fx_btn_h(&L);
        if (d >= y1 && d < y1 + hb)
            return xx < (L.cw - 4) / 2 ? Z_REV : Z_HALF;
        if (d >= y1 + hb + 1 && d < y1 + 2 * hb + 1)
            return xx < (L.cw - 4) / 2 ? Z_MUTE : Z_UNDO;
        return Z_NONE;
    }
    if (d >= L.sel_y && d < L.sel_y + 13)
        return Z_SEL;
    if (d >= L.undo_y && d < L.undo_y + 12)
        return Z_UNDO;
    if (d >= L.sel_y + 16 && d < L.undo_y - 12) {
        float v = (float)(L.bars_y + L.bars_h - d) / (float)L.bars_h;
        *val = gain_of(v);                                        /* the gain at that height */
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

static float log2_fast(float x)
{
    union {
        float f;
        uint32_t u;
    } v = {x};
    float e = (float)((int)(v.u >> 23) - 127);
    v.u = (v.u & 0x7fffffu) | 0x3f800000u;                       /* the mantissa, 1..2 */
    return e + (-0.34484843f * v.f + 2.02466578f) * v.f - 0.67487759f;
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

static char *put_hex(char *p, uint32_t v, int digits)
{
    for (int i = digits - 1; i >= 0; i--)
        *p++ = "0123456789abcdef"[(v >> (4 * i)) & 15];
    return p;
}

/* A gain as dB with one decimal: "0.0dB", "-6.0dB", "+3.5dB", or "-inf". At most 8 characters. */
static char *put_db(char *p, float g)
{
    if (g < 0.01f) {
        *p++ = '-';
        *p++ = 'i';
        *p++ = 'n';
        *p++ = 'f';
        return p;
    }
    float db = 6.0206f * log2_fast(g);
    int tenths = (int)((db < 0.f ? -db : db) * 10.f + .5f);
    if (tenths)
        *p++ = db < 0.f ? '-' : '+';
    p = put_uint(p, (unsigned)tenths / 10);
    *p++ = '.';
    p = put_uint(p, (unsigned)tenths % 10);
    *p++ = 'd';
    *p++ = 'B';
    return p;
}

static const char *state_name(const struct looper_info *k)
{
    if (k->armed)
        return "ARMED";
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

/* A percentage, "0".."100" */
static char *put_pct(char *p, float v)
{
    int n = (int)(v * 100.f + .5f);
    return put_uint(p, (unsigned)(n < 0 ? 0 : n > 100 ? 100 : n));
}

static float fx_value(const struct looper_info *k, int param)
{
    return param == LOOPER_P_FILT ? k->filt : param == LOOPER_P_RES ? k->res : param == LOOPER_P_CRUNCH ? k->crunch :
           param == LOOPER_P_DRIVE ? k->drive : param == LOOPER_P_SEND_D ? k->send_d : k->send_r;
}

static const char *const fx_name[6] = {"FILT", "RES", "CRSH", "DRIVE", "DLY", "RVB"};

/* The text a knob shows on top: "LVL 0.0dB" / the pan line / the selected FX parameter's value. */
static void fx_text(char *b, int param, float v)
{
    char *p = b;
    for (const char *s = fx_name[param]; *s; s++)
        *p++ = *s;
    *p++ = ' ';
    if (param == LOOPER_P_FILT) {
        if (v > -.03f && v < .03f) {
            *p++ = 'O';
            *p++ = 'F';
            *p++ = 'F';
        } else {
            *p++ = v < 0.f ? 'L' : 'H';
            *p++ = 'P';
            *p++ = ' ';
            p = put_pct(p, v < 0.f ? -v : v);
        }
    } else {
        p = put_pct(p, v);
    }
    *p = 0;
}

static float opt_get(int o)
{
    return looper_get_opt(o);
}

static void draw_top(int t, const struct lay *L, const struct looper_info *k)
{
    int x = L->cx, w = L->cw;
    box(x, 1, w, TOPBAR, C_BG);
    char b[24], *p = b;
    if (P->mode == M_SETUP || P->mode == M_MORE) {                /* what the knobs turn */
        static const char *const nm_s[4] = {"GAIN ", "", "", ""};
        static const char *const nm_m[4] = {"FB ", "RET ", "SIZE ", "RET "};
        static const uint8_t op_s[4] = {LOOPER_O_GAIN, 0, 0, 0};
        static const uint8_t op_m[4] = {LOOPER_O_DFB, LOOPER_O_DRET, LOOPER_O_RSIZE, LOOPER_O_RRET};
        const char *nm = P->mode == M_SETUP ? nm_s[t] : nm_m[t];
        for (const char *q = nm; *q; q++)
            *p++ = *q;
        if (*nm)
            p = put_pct(p, opt_get(P->mode == M_SETUP ? op_s[t] : op_m[t]));
    } else if (P->mode == M_FX) {
        fx_text(b, P->fx_sel[t], fx_value(k, P->fx_sel[t]));
        p = b;
        while (*p)
            p++;
    } else {
        for (const char *s = "LVL "; *s; s++)
            *p++ = *s;
        p = put_db(p, k->level);
    }
    *p = 0;
    text_c(x, 4, w, b, C_WHITE, 1);
}

/* The record box and its contents: the same on the MAIN and FX tabs. */
static void draw_rec(int t, const struct lay *L, const struct looper_info *k)
{
    int x = L->cx, w = L->cw, colour = track_colour[t];
    int live = k->mode == LOOPER_PLAY || k->mode == LOOPER_DUB;
    int scol = k->armed ? C_YELLOW : k->mode == LOOPER_REC ? C_REC : k->mode == LOOPER_DUB ? C_YELLOW : k->mode == LOOPER_PLAY ? (k->muted ? C_RED : C_GREEN)
             : k->mode == LOOPER_EMPTY ? C_RAIL : C_WHITE;
    int rx = x + 3, rw = w - 6, ry = L->rec_y, rh = L->rec_h;
    frame(rx, ry, rw, rh, scol, 1);
    if (k->latched || k->mode == LOOPER_REC)
        frame(rx + 2, ry + 2, rw - 4, rh - 4, k->latched ? C_WHITE : scol, 1);     /* a second line, like a pressed button */
    char num[2] = {(char)('1' + t), 0};
    text(rx + 7, ry + (rh >= 30 ? 5 : 3), num, k->mode == LOOPER_EMPTY && !k->armed ? C_GREY : colour, 2);
    text(rx + 24, ry + 9, state_name(k), k->mode == LOOPER_EMPTY && !k->armed ? C_GREY : scol, 1);
    if (k->half)
        text(rx + rw - 22, ry + (rh >= 30 ? 20 : 9), "1/2", C_YELLOW, 1);
    if (rh >= 30)
        icon(L, k, scol);
    if (live || k->mode == LOOPER_REC) {                          /* the playhead, a hairline along the bottom */
        int px = (int)(k->progress * (float)(rw - 4));
        hline(rx + 2, ry + rh - 4, px, colour);
        if (rh >= 30)
            dot(rx + 2 + px, ry + rh - 4, colour);
    }
}

static void draw_column(int t, const struct lay *L, const struct looper_info *k)
{
    int x = L->cx, w = L->cw, colour = track_colour[t];
    int live = k->mode == LOOPER_PLAY || k->mode == LOOPER_DUB;
    box(x, L->col_y, w, L->col_h, C_BG);
    draw_rec(t, L, k);
    /* the fader: a hairline with ticks, a 2 px line in the track colour up to a round handle */
    int d = L->bars_y, h = L->bars_h, fx = x + w / 2;
    float gl = k->pan > 0.f ? 1.f - k->pan : 1.f, gr = k->pan < 0.f ? 1.f + k->pan : 1.f;
    vline(fx, d, h, C_RAIL);
    for (int q = 0; q <= 4; q++)
        hline(fx - 3, d + q * (h - 1) / 4, 7, C_GREY);
    int hy = d + h - (int)(pos_of(k->level) * (float)h + .5f);
    int lc = k->muted ? C_GREY : colour;
    box(fx, hy, 2, d + h - hy, lc);
    for (int c = 0; c < 2; c++) {                                 /* a thin meter for each output */
        int mx = c ? x + w - 9 : x + 8;
        int m = live ? (int)(pos_of(k->level * (c ? gr : gl)) * (float)h + .5f) : 0;
        vline(mx, d, h, C_RAIL);
        vline(mx, d + h - m, m, lc);
    }
    dot(fx, hy, C_WHITE);
    /* the level, small */
    char lv[12], *q = put_db(lv, k->level);
    *q = 0;
    text_c(x, L->lvl_y, w, lv, k->muted ? C_GREY : C_LIGHT, 1);
    int sc = P->sel == t ? C_PINK : C_RAIL;                       /* SELECT: the hardware REC button records this track */
    frame(x + 2, L->sel_y, w - 4, 13, sc, 1);
    text_c(x + 2, L->sel_y + 3, w - 4, P->sel == t ? "SELECTED" : "SELECT", P->sel == t ? C_PINK : C_GREY, 1);
    /* UNDO says what the next press (and the BACK button) will do: take the last pass off, or delete the loop */
    int uc = k->undo_kind == 2 ? C_RED : k->undo_kind == 1 ? C_YELLOW : C_RAIL;
    frame(x + 2, L->undo_y, w - 4, 12, uc, 1);
    text_c(x + 2, L->undo_y + 2, w - 4, k->undo_kind == 2 ? "DELETE LOOP" : k->undo_kind == 1 ? "UNDO PASS" : "UNDO", uc == C_RAIL ? C_GREY : uc, 1);
    /* the pan dial and the buttons */
    int by = L->btn_y, dx = x + 2, rbw = w - 4 - DIAL - 1, bx = dx + DIAL + 1, hh = BTN / 2;
    dial(dx, by, "PAN", (k->pan + 1.f) * .5f, (((P->pan_sel >> t) & 1) || P->info_on) != 0);
    int rc = k->reversed ? C_YELLOW : C_GREY;
    frame(bx, by, rbw, hh, rc, 1);
    text_c(bx, by + 5, rbw, "REV", k->reversed ? C_YELLOW : C_LIGHT, 1);
    int mc = !live ? C_RAIL : k->muted ? C_RED : C_GREEN;
    frame(bx, by + hh, rbw, BTN - hh, mc, 1);
    text_c(bx, by + hh + 5, rbw, "MUTE", !live ? C_GREY : mc, 1);
}

/* FX tab: the record box, six dials (FILT RES / CRSH DRIVE / DLY RVB), REV and HALF, MUTE. */
static void draw_column_fx(int t, const struct lay *L, const struct looper_info *k)
{
    int x = L->cx, w = L->cw;
    int live = k->mode == LOOPER_PLAY || k->mode == LOOPER_DUB;
    box(x, L->col_y, w, L->col_h, C_BG);
    draw_rec(t, L, k);
    int sc = P->sel == t ? C_PINK : C_RAIL;                       /* SELECT: INFO cycles this track's dial; BACK / REC act on it */
    frame(x + 2, L->sel_y, w - 4, 13, sc, 1);
    text_c(x + 2, L->sel_y + 3, w - 4, P->sel == t ? "SELECTED" : "SELECT", P->sel == t ? C_PINK : C_GREY, 1);
    int y0 = fx_top(L);
    for (int p = 0; p < 6; p++) {
        float v = fx_value(k, p);
        dial(x + 2 + (p & 1) * (DIAL + 1), y0 + (p >> 1) * (BTN + 1), fx_name[p], p == LOOPER_P_FILT ? (v + 1.f) * .5f : v,
             P->fx_sel[t] == p);
    }
    int y1 = y0 + 3 * (BTN + 1), bw = (w - 4) / 2 - 1, hb = fx_btn_h(L), ty = (hb - 8) / 2;
    int rc = k->reversed ? C_YELLOW : C_GREY, hc = k->half ? C_YELLOW : C_GREY;
    frame(x + 2, y1, bw, hb, rc, 1);
    text_c(x + 2, y1 + ty, bw, "REV", k->reversed ? C_YELLOW : C_LIGHT, 1);
    frame(x + 3 + bw, y1, bw, hb, hc, 1);
    text_c(x + 3 + bw, y1 + ty, bw, "HALF", k->half ? C_YELLOW : C_LIGHT, 1);
    int mc = !live ? C_RAIL : k->muted ? C_RED : C_GREEN;
    frame(x + 2, y1 + hb + 1, bw, hb, mc, 1);
    text_c(x + 2, y1 + hb + 1 + ty, bw, "MUTE", !live ? C_GREY : mc, 1);
    int uc = k->undo_kind == 2 ? C_RED : k->undo_kind == 1 ? C_YELLOW : C_RAIL;
    frame(x + 3 + bw, y1 + hb + 1, bw, hb, uc, 1);
    text_c(x + 3 + bw, y1 + hb + 1 + ty, bw, k->undo_kind == 2 ? "DEL" : "UNDO", uc == C_RAIL ? C_GREY : uc, 1);
}

/* SETUP and MORE tabs: the option rows, then a row of buttons. */
static void draw_setup(const struct lay *L)
{
    box(1, L->col_y, P->w - 2, L->col_h, C_BG);
    const struct optrow *rows = page_rows();
    for (int r = 0; r < OPT_ROWS; r++) {
        const struct optrow *R = &rows[r];
        int d = row_d(L, r);
        text(6, d + 5, R->name, C_LIGHT, 1);
        if (R->n) {
            int cur = (int)(opt_get(R->opt) + .5f);
            for (int c = 0; c < R->n; c++) {
                int on = c == cur;
                frame(OPT_X + c * CHOICE_W, d, CHOICE_W - 3, ROW_H - 4, on ? C_CYAN : C_RAIL, 1);
                text_c(OPT_X + c * CHOICE_W, d + 5, CHOICE_W - 3, R->c[c], on ? C_WHITE : C_GREY, 1);
            }
        } else {
            float v = opt_get(R->opt);
            int len = (int)(v * (float)SLIDER_W + .5f), my = d + (ROW_H - 4) / 2;
            hline(OPT_X, my, SLIDER_W, C_RAIL);
            for (int q = 0; q <= 4; q++)
                vline(OPT_X + q * (SLIDER_W - 1) / 4, my - 3, 7, C_GREY);
            hline(OPT_X, my, len, C_CYAN);
            dot(OPT_X + len, my, C_WHITE);
            char b[10], *p = b;
            if (R->opt == LOOPER_O_GAIN)
                p = put_db(p, 1.f + 3.f * v);
            else
                p = put_pct(p, v);
            *p = 0;
            text(OPT_X + SLIDER_W + 8, d + 5, b, C_LIGHT, 1);
        }
    }
    int d = row_d(L, OPT_ROWS);
    char b[64], *p = b;
    if (P->mode == M_SETUP) {
        int armed = P->clear_arm && looper_ticks() - P->clear_t < CLEAR_WAIT;
        frame(OPT_X, d, 110, ROW_H - 4, C_RED, 1);
        text_c(OPT_X, d + 5, 110, armed ? "PRESS AGAIN" : "CLEAR ALL", armed ? C_WHITE : C_RED, 1);
        float bpm = looper_bpm();
        for (const char *q = "BPM "; *q; q++)
            *p++ = *q;
        if (bpm > 0.f)
            p = put_uint(p, (unsigned)(bpm + .5f));
        else
            *p++ = '-';
        for (const char *q = looper_running() ? " RUN" : " STOP"; *q; q++)
            *p++ = *q;
        *p = 0;
        text(OPT_X + 120, d + 5, b, bpm > 0.f ? C_LIGHT : C_GREY, 1);
    } else {
        static const char *const nm[BTNS] = {"", "FX", "REC", "BACK", "STOP", "PLAY"};
        for (int i = 0; i < 6; i++) {                             /* two rows of three buttons */
            int slot = i < 5 ? i + 1 : 0, x = 6 + (i % 3) * 102, y = d + (i / 3) * ROW_H;
            char lb[16], *q = lb;
            if (slot) {
                const char *z = P->learn == slot ? "PRESS " : P->bset[slot] ? "" : "LEARN ";
                for (; *z; z++)
                    *q++ = *z;
                for (const char *w = nm[slot]; *w; w++)
                    *q++ = *w;
                if (P->learn != slot && P->bset[slot]) {
                    for (const char *w = " LEARNT"; *w; w++)
                        *q++ = *w;
                }
            } else {
                for (const char *z = "STOCK FX >"; *z; z++)
                    *q++ = *z;
            }
            *q = 0;
            int on = slot ? P->bset[slot] : P->bset[B_FX];
            int col = slot && P->learn == slot ? C_PINK : on ? C_GREEN : C_RAIL;
            frame(x, y, 96, ROW_H - 4, col, 1);
            text_c(x, y + 5, 96, lb, col == C_RAIL ? C_LIGHT : col, 1);
        }
        d += ROW_H;
        /* the last button message the page saw, the frame buffer's size and the touch probe, on one line */
        for (const char *q = "LAST "; *q; q++)
            *p++ = *q;
        p = put_uint(p, P->btn_id);
        *p++ = ':';
        p = put_uint(p, P->btn_index);
        for (const char *q = " FB "; *q; q++)
            *p++ = *q;
        p = put_uint(p, *(const uint16_t *)((const uint8_t *)FB + 4));
        *p++ = 'x';
        p = put_uint(p, *(const uint16_t *)((const uint8_t *)FB + 6));
        for (const char *q = " DROP "; *q; q++)
            *p++ = *q;
        p = put_uint(p, P->dropped);
        for (const char *q = " T "; *q; q++)
            *p++ = *q;
        p = put_uint(p, P->touches);
        for (const char *q = " X "; *q; q++)
            *p++ = *q;
        p = put_uint(p, (unsigned)(P->touch_x < 0 ? 0 : P->touch_x));
        for (const char *q = " Y "; *q; q++)
            *p++ = *q;
        p = put_uint(p, (unsigned)(P->touch_y < 0 ? 0 : P->touch_y));
        *p = 0;
        text(6, d + ROW_H + 2, b, C_GREY, 1);
        char ev[64], *z = ev;                                     /* the last engine events: which one is PLAY / STOP / REC? */
        *z++ = 'E';
        *z++ = 'V';
        for (int i = 0; i < 3; i++) {
            *z++ = ' ';
            z = put_hex(z, P->ev0[i] & 0xffffff, 6);
            *z++ = '.';
            z = put_hex(z, P->ev1[i] & 0xffff, 4);
        }
        *z = 0;
        text(6, d + ROW_H + 12, ev, C_GREY, 1);
    }
}

static void draw_footer(const struct lay *L)
{
    box(1, L->foot_y + 1, P->w - 2, FOOT - 1, C_BG);
    static const char *const tab[MODES] = {"MAIN", "FX", "SETUP", "MORE"};
    for (int i = 0; i < MODES; i++) {
        int on = P->mode == i;
        frame(3 + i * (TAB_W + 2), L->foot_y + 2, TAB_W, FOOT - 3, on ? C_CYAN : C_RAIL, 1);
        text_c(3 + i * (TAB_W + 2), L->foot_y + 4, TAB_W, tab[i], on ? C_CYAN : C_GREY, 1);
    }
    int x0 = 3 + MODES * (TAB_W + 2) + 4;
    char b[48], *p = b;
    if (looper_len()) {
        unsigned len = looper_len() * 10u / 48000u, pos = (unsigned)(looper_progress() * (float)looper_len()) * 10u / 48000u;
        (void)pos;
        const char *s = "LOOP ";
        while (*s)
            *p++ = *s++;
        p = put_uint(p, len / 10);
        *p++ = '.';
        p = put_uint(p, len % 10);
        *p++ = 's';
    } else {
        const char *s = "";
        while (*s)
            *p++ = *s++;
    }
    *p = 0;
    text(x0, L->foot_y + 4, b, C_LIGHT, 1);
    if (looper_paused())
        text(x0 + 60, L->foot_y + 4, "PAUSED", C_RED, 1);
    if (!P->info_set)
        text(P->w - 5 - text_w("PRESS INFO", 1), L->foot_y + 4, "PRESS INFO", C_PINK, 1);
    else if (P->info_on)
        text(P->w - 5 - text_w("SHIFT ON", 1), L->foot_y + 4, "SHIFT ON", C_RED, 1);
}

/* A number that changes whenever something the page shows changes. */
static uint32_t signature(void)
{
    uint32_t h = 2166136261u;
    for (int t = 0; t < LOOPER_TRACKS; t++) {
        struct looper_info k;
        looper_track(t, &k);
        uint32_t v = (uint32_t)k.mode | (uint32_t)k.muted << 4 | (uint32_t)k.reversed << 5 | (uint32_t)k.latched << 6 |
                     (uint32_t)k.armed << 7 | (uint32_t)(k.level * 100.f + .5f) << 8 | (uint32_t)(k.pan * 100.f + 100.5f) << 16;
        h = (h ^ v) * 16777619u;
        v = (uint32_t)k.half | (uint32_t)(k.filt * 100.f + 100.5f) << 1 | (uint32_t)(k.crunch * 100.f + .5f) << 10 |
            (uint32_t)(k.send_d * 100.f + .5f) << 17 | (uint32_t)(k.send_r * 100.f + .5f) << 24;
        h = (h ^ v) * 16777619u;
        h = (h ^ ((uint32_t)(k.res * 100.f + .5f) | (uint32_t)(k.drive * 100.f + .5f) << 8 | (uint32_t)k.undo_kind << 16)) * 16777619u;
        h = (h ^ (uint32_t)(k.progress * 400.f)) * 16777619u;          /* each playhead, in 1/400ths of its loop */
    }
    h = (h ^ ((uint32_t)P->pan_sel | (uint32_t)P->info_on << 8 | (uint32_t)P->info_set << 16 | (uint32_t)P->info_id << 17)) * 16777619u;
    h = (h ^ (uint32_t)(looper_progress() * 400.f)) * 16777619u;     /* the master playhead */
    h = (h ^ ((uint32_t)P->mode | (uint32_t)P->fx_sel[0] << 4 | (uint32_t)P->fx_sel[1] << 8 | (uint32_t)P->fx_sel[2] << 12 |
              (uint32_t)P->fx_sel[3] << 16 | (uint32_t)looper_running() << 20 | (uint32_t)looper_bpm() << 21 | (uint32_t)P->learn << 27 | (uint32_t)looper_paused() << 30 | (uint32_t)P->bset[1] << 26 | (uint32_t)P->bset[2] << 25 | (uint32_t)P->bset[3] << 24 | (uint32_t)P->bset[4] << 23 | (uint32_t)P->bset[5] << 22 |
              (uint32_t)(P->clear_arm && looper_ticks() - P->clear_t < CLEAR_WAIT) << 30)) * 16777619u;
    for (int o = 0; o < LOOPER_OPTS; o++)
        h = (h ^ (uint32_t)(looper_get_opt(o) * 1000.f + .5f)) * 16777619u;
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
    paint(view);
    return 1;
}

/* The whole page, into the frame buffer the last stock draw pass handed over. */
static void paint(uint8_t *view)
{
    geometry(view);
    rehide(view);
    widen(view);
    looper_guard(looper_get_opt(LOOPER_O_FULL) > .5f ? 1 : 2);   /* from now on the stock line / text drawing is dropped */
    looper_guard_drawing(1);
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
        if (P->mode == M_FX)
            draw_column_fx(t, &L, &k);
        else if (P->mode == M_MAIN)
            draw_column(t, &L, &k);
        if (t < LOOPER_TRACKS - 1)
            vline(L.cx + L.cw, 1, (P->mode >= M_SETUP ? TOPBAR : L.foot_y) - 1, C_RAIL);
    }
    layout(0, &L);
    if (P->mode == M_SETUP || P->mode == M_MORE)
        draw_setup(&L);
    draw_footer(&L);
    P->sig = signature();
    looper_guard_drawing(0);
}

static void dirty(uint8_t *view)
{
    for (int i = 0; i < 16; i++)
        cell_at(view, i)[CELL_DIRTY] = 1;
    P->force = 0;
}

/* From the GUI task (touches, knobs): mark dirty and also paint at once, so the page shows what was just done even when
 * no stock draw pass follows (the stock widgets that used to ask for one are out of the way). */
static void dirty_now(uint8_t *view)
{
    dirty(view);
    if (P->fb && solo_looper_view() == view)
        paint(view);
}

/* From the audio task (looper_ui_poke -> solo.c): redraw when something changed, and now and then regardless. */
void looper_page_poke(uint8_t *view)
{
    if (P->info_on && looper_ticks() - P->info_t > INFO_TIMEOUT) {
        P->info_on = 0;
        P->sig = 0;
    }
    if (signature() != P->sig || ++P->force >= 40) {
        dirty(view);
        /* With the stock screen dropped (full screen) nothing else asks the GUI for a draw pass: post our own message,
         * handled in the GUI task by looper_app_msg. One in flight at a time. */
        if (looper_get_opt(LOOPER_O_FULL) > .5f && P->fb &&
            (!P->paint_req || looper_ticks() - P->paint_t > 60)) {
            uint16_t m[12];
            for (int i = 0; i < 12; i++)
                m[i] = 0;
            m[0] = MSG_PAINT;
            P->paint_req = 1;
            P->paint_t = looper_ticks();
            fw_post((void *)(0x24020088u + 0x30u), m);
        }
    }
}

/* The stock cells still draw their child widgets (the cyan double boxes under each pad) after the page: hide them
 * while the page shows. A base widget's hidden flag is its byte +0x30; the cell's children sit at +0x3c / +0x70 /
 * +0xc8 / +0x120. Their own flags are saved and put back when the page is left. */
static const uint16_t cell_children[4] = {0x6c, 0xa0, 0xf8, 0x150};

static void hide_children(uint8_t *view, int hide)
{
    if (hide == P->hid_on)
        return;
    for (int i = 0; i < 16; i++) {
        for (int k = 0; k < 4; k++) {
            uint8_t *flag = cell_at(view, i) + cell_children[k];
            uint32_t bit = 1u << ((i * 4 + k) & 31);
            volatile uint32_t *word = &P->hid[(i * 4 + k) >> 5];
            if (hide) {
                *word = (*word & ~bit) | (*flag ? bit : 0);
                *flag = 1;
            } else {
                *flag = (*word & bit) ? 1 : 0;
            }
        }
    }
    P->hid_on = (uint8_t)hide;
}

/* The stock code can show the cells' children again (a cell's own update toggles them): hide them at every draw. */
static void rehide(uint8_t *view)
{
    if (!P->hid_on)
        return;
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 4; k++)
            cell_at(view, i)[cell_children[k]] = 1;
}

/* Touches reach the view only inside its own rectangle (the 256 px of cells); the page covers the whole screen, so the
 * view's rectangle is widened to the page's while it shows, and put back on leaving. */
static void widen(uint8_t *view)
{
    int *r = (int *)(view + 4);
    if (!P->rect_on) {
        if (r[2] <= 0 || r[3] <= 0)
            return;
        for (int i = 0; i < 4; i++)
            P->vrect[i] = r[i];
        P->rect_on = 1;
    }
    r[0] = 0;
    r[2] = 2 * P->x0 + P->w;
}

/* The Looper page is no longer showing (the mixer view was shown in another mode). */
void looper_page_leave(uint8_t *view)
{
    looper_guard(0);
    hide_children(view, 0);
    if (P->rect_on) {
        int *r = (int *)(view + 4);
        for (int i = 0; i < 4; i++)
            r[i] = P->vrect[i];
        P->rect_on = 0;
    }
}

/* The Looper page has just been (re)shown. */
void looper_page_enter(uint8_t *view)
{
    hide_children(view, 1);
    P->entered = 0;
    P->pressed = P_NONE;
    P->info_on = 0;
    P->sig = 0;
    P->force = 0;
}

/* looper_boot: reset the page; the learned INFO button is kept while the backup SRAM is. (The cells are built at
 * boot with their stock flags, so nothing is left hidden.) */
void looper_page_boot(void)
{
    if (P->magic != PMAGIC) {
        P->magic = PMAGIC;
        P->info_set = 0;
    }
    looper_guard(0);
    looper_guard_drawing(0);
    P->info_set = 1;                                         /* INFO is message 0xc (seen as "INFO=12" on the unit);   */
    P->info_id = 0xc;                                        /* the backup SRAM does not survive a power cycle, so  */
    P->info_idx = 0;                                         /* nothing is learned by pressing any more              */
    for (int i = 0; i < BTNS; i++)
        P->bset[i] = 0;
    static const uint8_t pre_idx[BTNS] = {0, 4, 8, 11, 9, 10};                 /* as read off the unit: */
    static const uint16_t pre_id[BTNS] = {0, 0xf9, 0xf4, 7, 0xf6, 0xf7};      /* FX 249:4, REC 244:8, BACK 7:11, STOP 246:9, PLAY 247:10 */
    for (int i = 1; i < BTNS; i++) {
        P->bset[i] = 1;
        P->bid[i] = pre_id[i];
        P->bidx[i] = pre_idx[i];
    }
    P->paint_req = 0;
    P->dropped = 0;
    for (int i = 0; i < 4; i++)
        P->ev0[i] = P->ev1[i] = 0;
    P->learn = 0;
    P->touches = 0;
    P->sel = 0;
    P->hid_on = 0;
    P->rect_on = 0;
    P->clear_arm = 0;
    P->mode = M_MAIN;
    for (int t = 0; t < LOOPER_TRACKS; t++)
        P->fx_sel[t] = 0;
    P->_legacy = -1;
    P->hg = 0;
    P->pan_sel = 0;
    P->btn_id = P->btn_index = 0;
    P->entered = 0;
    P->pressed = P_NONE;
    P->info_on = 0;
    P->sig = 0;
    P->force = 0;
}

/* ---- touch and knobs */

/* Touch points use the same space as the cells: convert to pixels from the page's left and top. */
static void to_page(const int *pt, int *x, int *d)
{
    *x = P->sx > 0 ? pt[0] - P->x0 : P->x0 + P->w - pt[0];
    *d = P->s > 0 ? P->ytop - pt[1] : pt[1] - P->ytop;
    P->touch_x = (int16_t)pt[0];                                  /* shown on MORE: does a touch reach the page? */
    P->touch_y = (int16_t)pt[1];
    P->touches++;
    *x = *x < 1 ? 1 : *x > P->w - 2 ? P->w - 2 : *x;              /* a tap on the very edge counts as the outermost column */
}

static void info_touch(void)
{
    P->info_t = looper_ticks();
}

static float fx_of(int t, int param)
{
    return looper_get_param(t, param);
}

static void set_fx(int t, int param, float v)
{
    looper_set_param(t, param, v);
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
    int was_sel = P->sel == t;
    if (zone != Z_TAB && zone != Z_OPTC && zone != Z_SLIDER && zone != Z_CLEAR)
        P->sel = (uint8_t)t;                                      /* touching a track's column selects it */
    if (P->info_on)
        info_touch();
    if (zone == Z_TAB) {
        P->mode = (uint8_t)t;
        P->info_on = 0;
        P->entered = 0;                                           /* repaint the whole page for the new tab */
        P->sig = 0;
    } else if (zone == Z_OPTC) {
        if (t >= 95 && t <= 99) {
            int slot = 100 - t;                                   /* LEARN <button>: the next press of it is remembered */
            P->learn = P->learn == slot ? 0 : (uint8_t)slot;
        } else if (t == 94) {
            if (P->bset[B_FX]) {                                  /* hand the FX button's message to the stock handler: its FX page */
                uint16_t m[32];
                for (int i = 0; i < 32; i++)
                    m[i] = 0;
                m[0] = P->bid[B_FX];
                *(uint32_t *)((uint8_t *)m + 0xc) = P->bidx[B_FX];
                fw_app_msg((void *)0x24020088u, m);
            }
        } else {
            looper_set_opt(t, v);
            P->entered = 0;                                       /* e.g. FULL SCREEN: repaint the whole background */
        }
    } else if (zone == Z_SLIDER) {
        looper_set_opt(t, v);
        P->drag_opt = (uint8_t)t;
        P->pressed = P_SLIDER;
    } else if (zone == Z_CLEAR) {
        if (P->clear_arm && looper_ticks() - P->clear_t < CLEAR_WAIT) {
            P->clear_arm = 0;                                     /* asked twice within 3 s */
            looper_clear_all();
        } else {
            P->clear_arm = 1;
            P->clear_t = looper_ticks();
        }
    } else if (zone == Z_FX) {
        int p = (int)(v + .5f);
        P->fx_sel[t] = (uint8_t)p;
        P->drag_param = (uint8_t)p;
        P->drag_y = (int16_t)d;
        P->drag_v0 = (int16_t)(fx_of(t, p) * 1000.f);
        P->pressed = P_DIAL;
    } else if (zone == Z_SEL) {
        P->sel = (uint8_t)t;
    } else if (zone == Z_UNDO) {
        looper_event(t, LOOPER_EV_UNDO);
    } else if (zone == Z_HALF) {
        looper_event(t, LOOPER_EV_HALF);
    } else if (zone == Z_REC) {
        if (P->info_on) {                                         /* INFO + box: mute, hold = undo / erase */
            looper_event(t, LOOPER_EV_MUTE_DOWN);
            P->pressed = P_MUTE;
        } else {
            looper_event(t, LOOPER_EV_REC_DOWN);
            P->pressed = P_REC;
        }
    } else if (zone == Z_FADER) {
        if (was_sel) {                                            /* the first touch of an unselected track only selects it */
            looper_set_level(t, v);
            P->pressed = P_FADER;
        }
    } else if (zone == Z_PAN) {
        P->pan_sel ^= (uint8_t)(1u << t);
    } else if (zone == Z_REV) {
        looper_event(t, LOOPER_EV_REVERSE);
    } else {
        looper_event(t, LOOPER_EV_MUTE_DOWN);
        P->pressed = P_MUTE;
    }
    dirty_now(view);
}

void looper_page_move(uint8_t *view, const int *pt)
{
    if (P->pressed != P_FADER && P->pressed != P_DIAL && P->pressed != P_SLIDER)
        return;
    int x, d;
    to_page(pt, &x, &d);
    struct lay L;
    layout(P->track, &L);
    if (P->pressed == P_FADER) {
        looper_set_level(P->track, gain_of((float)(L.bars_y + L.bars_h - d) / (float)L.bars_h));
    } else if (P->pressed == P_DIAL) {                            /* 80 px of travel for the full range */
        float span = P->drag_param == LOOPER_P_FILT ? 2.f : 1.f;
        set_fx(P->track, P->drag_param, (float)P->drag_v0 * .001f + (float)(P->drag_y - d) * span / 80.f);
    } else {
        float v = (float)(x - OPT_X) / (float)SLIDER_W;
        looper_set_opt(P->drag_opt, v < 0.f ? 0.f : v > 1.f ? 1.f : v);
    }
    dirty_now(view);
}

void looper_page_up(uint8_t *view, const int *pt)
{
    (void)pt;
    if (P->pressed == P_REC)
        looper_event(P->track, LOOPER_EV_REC_UP);
    else if (P->pressed == P_MUTE)
        looper_event(P->track, LOOPER_EV_MUTE_UP);
    P->pressed = P_NONE;
    dirty_now(view);
}

/* Mixer view vtable +0x34 (0x080f0f44): its messages. On the page the four knobs turn the tracks' levels, or the pans
 * (the PAN button, or INFO held). */
void looper_view_msg(uint8_t *view, const uint16_t *msg)
{
    if (msg && msg[0] == MSG_KNOB && solo_looper_view() == view) {
        int knob = *(const int16_t *)((const uint8_t *)msg + 0xc);
        knob = knob < 2 ? knob ^ 1 : knob;                       /* left encoders: the bottom one is track 1 */
        int counts = *(const int16_t *)((const uint8_t *)msg + 0x10);
        if (knob >= 0 && knob < LOOPER_TRACKS) {
            struct looper_info k;
            looper_track(knob, &k);
            float step = (float)counts * KNOB_SCALE;
            if (P->mode == M_MAIN || P->mode == M_FX)
                P->sel = (uint8_t)knob;                                   /* altering a track's setting selects it */
            if (P->mode == M_SETUP || P->mode == M_MORE) {
                static const uint8_t op_s[4] = {LOOPER_O_GAIN, 0, 0, 0};
                static const uint8_t op_m[4] = {LOOPER_O_DFB, LOOPER_O_DRET, LOOPER_O_RSIZE, LOOPER_O_RRET};
                if (P->mode == M_MORE)
                    looper_set_opt(op_m[knob], looper_get_opt(op_m[knob]) + step);
                else if (knob == 0)
                    looper_set_opt(op_s[0], looper_get_opt(op_s[0]) + step);
            } else if (P->mode == M_FX) {
                int p = P->fx_sel[knob];
                set_fx(knob, p, fx_of(knob, p) + step * (p == LOOPER_P_FILT ? 2.f : 1.f));
            } else if (P->mode == M_MAIN && (((P->pan_sel >> knob) & 1) || P->info_on)) {
                looper_set_pan(knob, k.pan + step * 2.f);
                if (P->info_on)
                    info_touch();
            } else {
                looper_set_level(knob, gain_of(pos_of(k.level) + step));
            }
            dirty_now(view);
        }
        return;
    }
    fw_view_msg(view, msg);
}

/* The special button messages the app's dispatcher gets (FUN_080a2e60): 7 and 0xc switch between a screen's pages
 * (0x2f -> 0x2e: the "back to the normal mixer" INFO caused), 8 sets the info / shift state at app + 0xea9e, 0xf9
 * carries the eight main buttons (index 5 = MIX), 0xf4 / 0xf6 / 0xf7 clear that state. */
static int button_msg(unsigned id)
{
    return id == 7 || id == 8 || id == 0xc || id == MSG_BUTTON;
}

/* INFO: its press toggles INFO mode (on until pressed again, or 10 s idle). Releases are not reported to the app, so
 * there is no hold-to-use. Returns 1 when the message was INFO's and the stock handler must not see it. */
static int info_button(unsigned id, unsigned idx)
{
    if (!button_msg(id) || (id == MSG_BUTTON && (idx == BTN_MIX || idx > 7)))
        return 0;
    if (!P->info_set) {                                            /* the first such press is taken as INFO */
        P->info_set = 1;
        P->info_id = (uint16_t)id;
        P->info_idx = (uint8_t)idx;
    }
    if (id != P->info_id || (id == MSG_BUTTON && idx != P->info_idx))
        return 0;
    if (P->mode == M_FX) {                                         /* FX tab: INFO steps the selected track's dial on by one */
        P->fx_sel[P->sel & 3] = (uint8_t)((P->fx_sel[P->sel & 3] + 1) % 6);
        P->sig = 0;
        return 1;
    }
    P->info_on = !P->info_on;
    P->info_down = looper_ticks();
    P->info_t = P->info_down;
    P->sig = 0;
    return 1;
}

/* Replaces the app's message dispatch call (bl @0x080a23cc). */
void looper_app_msg(void *app, const uint16_t *msg)
{
    looper_note_app(app);
    if (msg && msg[0] == MSG_PAINT) {
        P->paint_req = 0;
        uint8_t *view = solo_looper_view();
        if (view && P->fb && ((const uint8_t *)app)[0x8ca4] == 0x2f)
            paint(view);
        return;
    }
    /* only while the mixer screen (0x2f) is the one showing: the Looper flag outlives a trip to other screens, and INFO
     * must stay the stock button there */
    if (msg && solo_looper_view() && ((const uint8_t *)app)[0x8ca4] == 0x2f) {
        unsigned id = msg[0], idx = *(const uint32_t *)((const uint8_t *)msg + 0xc);
        int special = button_msg(id) || (id >= 0xf4 && id <= 0xf9);
        if (special) {
            P->btn_id = (uint16_t)id;
            P->btn_index = (uint16_t)idx;
        }
        int is_mix = id == MSG_BUTTON && idx == BTN_MIX;
        int is_info = id == P->info_id && (id != MSG_BUTTON || idx == P->info_idx);
        if (special && !is_mix && !is_info) {
            if (P->learn) {                                       /* MORE: LEARN <button>, then press it */
                P->bset[P->learn] = 1;
                P->bid[P->learn] = (uint16_t)id;
                P->bidx[P->learn] = (uint8_t)idx;
                P->learn = 0;
                P->sig = 0;
                return;
            }
            for (int slot = 1; slot < BTNS; slot++) {
                if (!P->bset[slot] || id != P->bid[slot] || idx != P->bidx[slot])
                    continue;
                if (looper_ticks() - P->btn_t <= 20) {             /* a press and its release can both arrive: one action */
                    if ((slot == B_STOP || slot == B_PLAY) && looper_get_opt(LOOPER_O_HWBTN) > .5f)
                        break;
                    return;
                }
                P->btn_t = looper_ticks();
                switch (slot) {
                case B_FX:
                    P->mode = P->mode == M_FX ? M_MAIN : M_FX;     /* the FX button: the looper's FX, and back */
                    P->info_on = 0;
                    P->entered = 0;
                    break;
                case B_REC:
                    looper_event(P->sel, LOOPER_EV_REC_DOWN);      /* the selected track: a tap (the sequencer never sees REC here) */
                    looper_event(P->sel, LOOPER_EV_REC_UP);
                    break;
                case B_BACK:
                    looper_event(P->sel, LOOPER_EV_UNDO);          /* BACK: undo on the selected track */
                    break;
                case B_STOP:
                    looper_transport(LOOPER_T_STOP);
                    break;
                case B_PLAY:
                    looper_transport(LOOPER_T_PLAY);
                    break;
                }
                P->sig = 0;
                if ((slot == B_STOP || slot == B_PLAY) && looper_get_opt(LOOPER_O_HWBTN) > .5f)
                    break;                                         /* HW STOP PLAY +STOCK: the sequencer / clock gets them too */
                return;
            }
        }
        if (info_button(id, idx))
            return;
    }
    fw_app_msg(app, msg);
}

/* From solo.c: a stock line / text draw was dropped (shown on MORE, to tell whether the hooks fire). */
void looper_page_dropped(void)
{
    P->dropped++;
}

/* From solo.c: an event was queued for the audio engine while the page shows. Newest first, distinct ones only. */
void looper_page_event(uint32_t w0, uint32_t w1)
{
    for (int i = 0; i < 4; i++)
        if (P->ev0[i] == w0 && P->ev1[i] == w1)
            return;
    for (int i = 3; i > 0; i--) {
        P->ev0[i] = P->ev0[i - 1];
        P->ev1[i] = P->ev1[i - 1];
    }
    P->ev0[0] = w0;
    P->ev1[0] = w1;
}
