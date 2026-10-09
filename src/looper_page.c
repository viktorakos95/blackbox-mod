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
#include "samplr.h"

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
unsigned looper_screen_id(void);
float looper_rate(void);
unsigned looper_clk_pos(void);
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
#define FX_N 16                                                   /* the FX tab's controls, two pages of eight: six dials, REV, HALF; STAB RPT SPEED DROP TRIM STUT SCRM PTCH */
#define FX_PAGE(c) ((c) >> 3)
/* The square's tiles (0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right) lie like the encoders on the panel: left top is
 * track 2's (knob 1), left bottom track 1's (knob 0), right top track 3's, right bottom track 4's. */
static const uint8_t slot_knob[4] = {1, 2, 0, 3};                 /* tile slot -> knob */
static const uint8_t knob_slot[4] = {2, 0, 1, 3};                 /* knob -> tile slot */
#define FX_GROUP (P->fx_sel[0] & 3)                              /* the pink square: a 2 x 2 block of four controls = the four knobs; it is in track P->sel's column */
#define BTN       34             /* the bottom row: the pan dial and the REV / MUTE buttons */
#define DIAL      34             /* width of the pan dial's box */

#define INFO_TIMEOUT 1875        /* audio blocks (10 s) */
#define INFO_HOLD    84          /* 0.45 s */

enum { P_NONE, P_REC, P_MUTE, P_FADER, P_DIAL, P_SLIDER };
enum { Z_NONE, Z_REC, Z_FADER, Z_PAN, Z_REV, Z_MUTE, Z_TAB, Z_FX, Z_HALF, Z_OPTC, Z_SLIDER, Z_CLEAR, Z_UNDO, Z_SEL };
enum { M_MAIN, M_FX, M_SETUP, M_MORE, M_SMPLR, MODES };
#define SM_SHEETS 4
#define SMS_REC 1
#define SMS_FX 2
#define SMS_SMPL 3
#define SM_SHEET (P->_r2)
static const char *const sm_sheet_name[SM_SHEETS] = {"PLAY", "REC", "FX", "SMPL"};
#define NTABS 5                                                   /* MAIN FX FX2 SETUP MORE: FX2 is the FX tab on its second page (groups 2, 3) */
/* hardware buttons the page can take over: slot numbers */
enum { B_NONE, B_FX, B_REC, B_BACK, B_STOP, B_PLAY, BTNS };

#define TAB_W      34
#define ROW_H      22                      /* SETUP: one option per row */
#define OPT_X      84                      /* SETUP: where the choices start */
#define CHOICE_W   52
#define SLIDER_W   150
#define DTAP 64                                                   /* audio blocks (340 ms): two taps this close are a double tap */
#define CLEAR_SHOW 375                     /* audio blocks (2 s) to show CLEARED! */
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
    uint8_t mode, fx_sel[LOOPER_TRACKS];            /* the tab; fx_sel[0] = the FX group (the pink square's block of four controls) */
    uint8_t drag_param, drag_opt;                   /* a dial / slider being dragged */
    int16_t drag_y, drag_v0;                        /* dial drag: start y, start value x 1000 */
    uint8_t clear_arm, sel, learn, _r5;            /* CLEAR ALL asked once; the selected track; the slot being learned */
    uint16_t rect_on, touches;
    uint8_t bset[BTNS], bidx[BTNS];                 /* learned hardware buttons: set, button index ... */
    uint16_t bid[BTNS];                             /* ... and message id, per slot */
    uint32_t rec_t, btn_t, paint_t, dropped;
    uint8_t paint_req, _r6[3];
    int16_t touch_x, touch_y;
    uint32_t clear_t;                               /* when CLEAR ALL was asked */
    uint32_t dtap_t;                                /* the last tap on an FX tile: when, and which (control + 1, track) */
    uint8_t dtap_c, dtap_trk, tcur, tmax;           /* touch probe: fingers down now / most at once */
    int32_t tw2, tw3;                               /* the touch event's third and fourth words (a finger id?) */
    int32_t vrect[4];                               /* the mixer view's own rectangle, while the page widens it */
    uint8_t scr_h[8];                               /* the last distinct screen ids the app showed (newest first) */
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
    {"QUANT", LOOPER_O_QUANT, 4, {"1/4", "1/8", "1/16", "1 BAR"}},
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
    {"MONITOR INPUT", LOOPER_O_MON, 0, {0, 0, 0, 0}},
    {"SPEED PITCH", LOOPER_O_PITCH, 3, {"TAPE", "GRAIN", "SMOOTH", 0}},
};
#define OPT_ROWS opt_rows()                                       /* rows above the buttons row: 7 on SETUP, 6 on MORE */
static int opt_rows(void);

static int opt_rows(void)
{
    return P->mode == M_MORE ? 8 : 7;
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

/* FX tab: the height of a control tile (four rows of two) and of the MUTE | UNDO row below them. */
static int fx_tile_h(const struct lay *L)
{
    int h = (L->col_y + L->col_h - fx_top(L) - 3 - 16) / 4 - 1;
    return h > BTN ? BTN : h < 24 ? 24 : h;
}

static int fx_btn_h(const struct lay *L)
{
    int y1 = fx_top(L) + 4 * (fx_tile_h(L) + 1), h = L->col_y + L->col_h - y1 - 3;
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
    if (d > L.foot_y) {                                           /* the footer: the tabs, and at the right the stock FX shortcut */
        if (x >= P->w - 58 && x < P->w - 4) {
            *track = 94;
            return Z_OPTC;
        }
        if (P->mode == M_SMPLR) {                                 /* the sheets (track >= 10 tells Z_TAB apart) */
            int sh = (x - 3) / 60;
            if (x >= 3 && sh < SM_SHEETS && (x - 3) % 60 < 58) {
                *track = 10 + sh;
                return Z_TAB;
            }
            return Z_NONE;
        }
        int i = (x - 3) / (TAB_W + 2);
        if (x >= 3 && i < NTABS && (x - 3) % (TAB_W + 2) < TAB_W) {
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
        if (0) {
            int b = (x - 6) / 102;
            if (x < 6 || b > 2 || (x - 6) % 102 >= 96)
                return Z_NONE;
            *track = 100 - ((r - OPT_ROWS) * 3 + b + 1);          /* 99 FX, 98 REC, 97 BACK, 96 STOP, 95 PLAY, 94 STOCK FX */
            return Z_OPTC;
        }
        if (r == OPT_ROWS) {
            if (P->mode == M_SETUP)
                return x >= OPT_X && x < OPT_X + 110 ? Z_CLEAR : Z_NONE;
#ifdef BANK2
            if (P->mode == M_MORE) {
                *track = 93;
                return Z_OPTC;
            }
#endif
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
        int th = fx_tile_h(&L);
        if (d >= y0 && d < y0 + 4 * (th + 1) && xx >= 0 && xx < 2 * (DIAL + 1) && (d - y0) % (th + 1) < th &&
            xx % (DIAL + 1) < DIAL) {
            int c = (FX_GROUP >> 1) * 8 + ((d - y0) / (th + 1)) * 2 + xx / (DIAL + 1);
            if (c >= FX_N)
                return Z_NONE;
            *val = (float)c;
            return Z_FX;
        }
        int y1 = y0 + 4 * (th + 1), hb = fx_btn_h(&L);
        if (d >= y1 && d < y1 + hb)
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
        return k->first ? "REC" : "DUB";
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
static void dial_h(int x, int d, int h, const char *name, float v, int selected, int on)
{
    frame(x, d, DIAL, h, selected ? C_PINK : on ? C_YELLOW : C_RAIL, 1);
    if (selected)
        frame(x + 1, d + 1, DIAL - 2, h - 2, C_PINK, 1);
    text_c(x, d + 3, DIAL, name, on ? C_YELLOW : C_LIGHT, 1);
    int cx = x + DIAL / 2, cy = d + h - 12;
    for (int i = 0; i < 28; i++)
        box(cx + arc[i][0], cy + arc[i][1], 1, 1, C_LIGHT);
    int i = (int)(v * 27.f + .5f);
    i = i < 0 ? 0 : i > 27 ? 27 : i;
    for (int k = 1; k <= 9; k++)                                  /* the pointer, from the centre */
        box(cx + arc[i][0] * k / 11, cy + arc[i][1] * k / 11, 1, 1, C_CYAN);
}

static void dial(int x, int d, const char *name, float v, int selected)
{
    dial_h(x, d, BTN, name, v, selected, 0);
}

/* A percentage, "0".."100" */
static char *put_pct(char *p, float v)
{
    int n = (int)(v * 100.f + .5f);
    return put_uint(p, (unsigned)(n < 0 ? 0 : n > 100 ? 100 : n));
}

static int fx_bipolar(int q)                                       /* engine parameters that go -1..1 */
{
    return q == LOOPER_P_FILT || q == LOOPER_P_SPEED || q == LOOPER_P_DROP || q == LOOPER_P_STUT || q == LOOPER_P_SCRM || q == LOOPER_P_PTCH;
}

static float fx_default(int q)
{
    return q == LOOPER_P_RES ? .5f : q == LOOPER_P_RPT ? 1.f : 0.f;
}

static int fxp(int c)                                             /* control -> engine parameter (6 and 7 are the REV / HALF switches) */
{
    return c >= 8 ? c - 2 : c;
}

static float fx_value(const struct looper_info *k, int param)
{
    if (param == 6 || param == 7)
        return (param == 6 ? k->reversed : k->half) ? 1.f : 0.f;
    if (param >= 8)
        return param == 8 ? k->stab : param == 9 ? k->rpt : param == 10 ? k->speed : param == 11 ? k->drop :
               param == 12 ? k->trim : param == 13 ? k->stut : param == 14 ? k->scrm : k->ptch;
    return param == LOOPER_P_FILT ? k->filt : param == LOOPER_P_RES ? k->res : param == LOOPER_P_CRUNCH ? k->crunch :
           param == LOOPER_P_DRIVE ? k->drive : param == LOOPER_P_SEND_D ? k->send_d : k->send_r;
}

static const char *const fx_name[FX_N] = {"FILT", "RES", "CRSH", "DRIVE", "DLY", "RVB", "REV", "HALF",
                                          "STAB", "RPT", "SPEED", "DROP", "TRIM", "STUT", "SCRM", "PTCH"};

/* The text a knob shows on top: "LVL 0.0dB" / the pan line / the selected FX parameter's value. */
static void fx_text(char *b, int param, float v)
{
    char *p = b;
    for (const char *s = fx_name[param]; *s; s++)
        *p++ = *s;
    *p++ = ' ';
    if (param == 6 || param == 7) {
        const char *o = v > .5f ? "ON" : "OFF";
        for (; *o; o++)
            *p++ = *o;
    } else if (param == 10) {                                     /* SPEED: the factor */
        float f = v >= 0.f ? 1.f + v : 1.f + 2.f * v;
        if (f < 0.f)
            *p++ = '-';
        int n = (int)((f < 0.f ? -f : f) * 10.f + .5f);
        p = put_uint(p, (unsigned)(n / 10));
        *p++ = '.';
        p = put_uint(p, (unsigned)(n % 10));
        *p++ = 'x';
    } else if (param == 12) {                                     /* TRIM: the part of the loop that plays */
        int n = (int)(v * 6.f + .5f);
        const char *o = n <= 0 ? "OFF" : n == 1 ? "1/2" : n == 2 ? "1/4" : n == 3 ? "1/8" : n == 4 ? "1/16" : n == 5 ? "1/32" : "1/64";
        for (; *o; o++)
            *p++ = *o;
    } else if (param == 11 || param == 14) {                      /* DROP / SCRM: random (left) / pattern (right) */
        float a = v < 0.f ? -v : v;
        if (a < .02f) {
            *p++ = 'O';
            *p++ = 'F';
            *p++ = 'F';
        } else {
            const char *o = v < 0.f ? "RND " : "SEQ ";
            for (; *o; o++)
                *p++ = *o;
            p = put_pct(p, a);
        }
    } else if (param == 15) {                                     /* PTCH: semitones */
        int semi = (int)(v * 24.f + (v >= 0.f ? .5f : -.5f));
        if (semi == 0) {
            *p++ = 'O';
            *p++ = 'F';
            *p++ = 'F';
        } else {
            *p++ = semi < 0 ? '-' : '+';
            p = put_uint(p, (unsigned)(semi < 0 ? -semi : semi));
        }
    } else if (param == 13) {                                     /* STUT: "<1/4" repeats what just played, ">1/4" what comes next */
        float a = v < 0.f ? -v : v;
        if (a < .02f) {
            *p++ = 'O';
            *p++ = 'F';
            *p++ = 'F';
        } else {
            static const char *const fr[6] = {"1", "1/2", "1/4", "1/8", "1/16", "1/32"};
            *p++ = v < 0.f ? '<' : '>';
            for (const char *o = fr[(int)(a * 5.99f)]; *o; o++)
                *p++ = *o;
        }
    } else if (param == LOOPER_P_FILT) {
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
        struct looper_info ks;                                    /* knob t turns control 4 * group + t of the selected track */
        looper_track(P->sel & 3, &ks);
        int c = FX_GROUP * 4 + knob_slot[t & 3];
        if (c < FX_N)
            fx_text(b, c, fx_value(&ks, c));
        else
            b[0] = 0;
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
    int scol = k->armed ? C_YELLOW : k->mode == LOOPER_REC || k->first ? C_REC : k->mode == LOOPER_DUB ? C_YELLOW : k->mode == LOOPER_PLAY ? (k->muted ? C_RED : C_GREEN)
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
static void set_fx_sel(int p)
{
    for (int t = 0; t < LOOPER_TRACKS; t++)
        P->fx_sel[t] = (uint8_t)p;
}

/* The stock FX page: the FX button's own message, handed to the app's dispatcher (the Blackbox goes to its FX page from here
 * as it does from the pads). Reached by the footer's STOCK FX button and by INFO + FX. */
static void stock_fx(void)
{
    uint16_t m[32];
    for (int i = 0; i < 32; i++)
        m[i] = 0;
    m[0] = P->bset[B_FX] ? P->bid[B_FX] : 0xf9;
    *(uint32_t *)((uint8_t *)m + 0xc) = P->bset[B_FX] ? P->bidx[B_FX] : 4;
    P->info_on = 0;
    fw_app_msg((void *)0x24020088u, m);
}

/* A tab of the footer (0 MAIN, 1 FX, 2 FX2, 3 SETUP, 4 MORE). FX / FX2 keep the square where it is when it is on that page. */
static void tab_select(int i)
{
    if (P->mode == M_SMPLR && i != 5)
        samplr_leave();
    if (i == 5 && P->mode != M_SMPLR)
        samplr_enter();
    P->mode = (uint8_t)(i == 0 ? M_MAIN : i <= 2 ? M_FX : i == 3 ? M_SETUP : i == 4 ? M_MORE : M_SMPLR);
    if (i == 1 && FX_GROUP >= 2)
        set_fx_sel(0);
    else if (i == 2 && FX_GROUP < 2)
        set_fx_sel(2);
}

static void draw_column_fx(int t, const struct lay *L, const struct looper_info *k)
{
    int x = L->cx, w = L->cw;
    int live = k->mode == LOOPER_PLAY || k->mode == LOOPER_DUB;
    box(x, L->col_y, w, L->col_h, C_BG);
    draw_rec(t, L, k);
    int sc = P->sel == t ? C_PINK : C_RAIL;                       /* SELECT: BACK / REC act on this track */
    frame(x + 2, L->sel_y, w - 4, 13, sc, 1);
    text_c(x + 2, L->sel_y + 3, w - 4, P->sel == t ? "SELECTED" : "SELECT", P->sel == t ? C_PINK : C_GREY, 1);
    int y0 = fx_top(L), th = fx_tile_h(L);
    int pg = FX_GROUP >> 1;
    for (int j = 0; j < 8; j++) {
        int p = pg * 8 + j;
        if (p >= FX_N)
            break;
        float v = fx_value(k, p);
        dial_h(x + 2 + (j & 1) * (DIAL + 1), y0 + (j >> 1) * (th + 1), th, fx_name[p],
               p == LOOPER_P_FILT || p == 10 || p == 11 || p == 13 || p == 14 || p == 15 ? (v + 1.f) * .5f : v, 0, (p == 6 || p == 7) && v > .5f);
    }
    if (P->sel == t) {                                            /* the one pink square: this column's block of four = knobs 1-4 */
        int by = y0 + (FX_GROUP & 1) * 2 * (th + 1) - 2;
        frame(x, by, 2 * (DIAL + 1) + 3, 2 * (th + 1) + 3, C_PINK, 1);
        frame(x + 1, by + 1, 2 * (DIAL + 1) + 1, 2 * (th + 1) + 1, C_PINK, 1);
        for (int i = 0; i < 4; i++) {                             /* which knob is which */
            char dg[2] = {(char)('1' + slot_knob[i]), 0};
            if (FX_GROUP * 4 + i < FX_N)
                text(x + 4 + (i & 1) * (DIAL + 1), y0 + ((FX_GROUP & 1) * 2 + (i >> 1)) * (th + 1) + th - 9, dg, C_PINK, 1);
        }
    }
    int y1 = y0 + 4 * (th + 1), bw = (w - 4) / 2 - 1, hb = fx_btn_h(L), ty = (hb - 8) / 2;
    int mc = !live ? C_RAIL : k->muted ? C_RED : C_GREEN;
    frame(x + 2, y1, bw, hb, mc, 1);
    text_c(x + 2, y1 + ty, bw, "MUTE", !live ? C_GREY : mc, 1);
    int uc = k->undo_kind == 2 ? C_RED : k->undo_kind == 1 ? C_YELLOW : C_RAIL;
    frame(x + 3 + bw, y1, bw, hb, uc, 1);
    text_c(x + 3 + bw, y1 + ty, bw, k->undo_kind == 2 ? "DEL" : "UNDO", uc == C_RAIL ? C_GREY : uc, 1);
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
        int armed = P->clear_arm == 1 && looper_ticks() - P->clear_t < CLEAR_WAIT;
        int done = P->clear_arm == 2 && looper_ticks() - P->clear_t < CLEAR_SHOW;
        frame(OPT_X, d, 110, ROW_H - 4, done ? C_GREEN : C_RED, 1);
        text_c(OPT_X, d + 5, 110, done ? "CLEARED!" : armed ? "TAP AGAIN" : "CLEAR ALL", done ? C_GREEN : armed ? C_WHITE : C_RED, 1);
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
        /* one diagnostic line: the last button message, the sequencer clock's position and rate, the screens visited */
        for (const char *q = "LAST "; *q; q++)
            *p++ = *q;
        p = put_uint(p, P->btn_id);
        *p++ = ':';
        p = put_uint(p, P->btn_index);
        for (const char *q = " CLK "; *q; q++)
            *p++ = *q;
        p = put_uint(p, looper_clk_pos());
        for (const char *q = " R "; *q; q++)
            *p++ = *q;
        p = put_uint(p, (unsigned)(looper_rate() * 1000.f));
        for (const char *q = " S"; *q; q++)
            *p++ = *q;
        for (int i = 0; i < 2; i++) {
            *p++ = ' ';
            p = put_hex(p, P->scr_h[i], 2);
        }
        *p++ = ' ';
        *p++ = 'T';
        p = put_uint(p, P->tmax);
        *p++ = ' ';
        *p++ = 'I';
        for (int i = 3; i >= 0; i--)                              /* the finger ids of the last four touch-downs, oldest first */
            p = put_hex(p, ((uint32_t)P->tw3 >> (4 * i)) & 0xfu, 1);
        *p++ = ' ';
        *p++ = 'C';                                               /* the audio task's load, average / worst block (cpu.c) */
        p = put_uint(p, (*(volatile uint16_t *)0x2405ffe2u + 5u) / 10u);
        *p++ = '/';
        p = put_uint(p, (*(volatile uint16_t *)0x2405ffe4u + 5u) / 10u);
#ifdef BANK2
        *p++ = ' ';
        *p++ = 'B';
        *p++ = '2';
        *p++ = ' ';
        if (!P->dropped) {
            *p++ = '?';
        } else if (P->dropped == 1) {
            *p++ = 'O';
            *p++ = 'K';
        } else {
            *p++ = 'B';
            *p++ = 'A';
            *p++ = 'D';
            p = put_uint(p, P->dropped - 2u);
        }
#endif
        *p = 0;
        text(6, d + 2, b, C_GREY, 1);
    }
}

static void draw_footer(const struct lay *L)
{
    box(1, L->foot_y + 1, P->w - 2, FOOT - 1, C_BG);
    static const char *const tab[NTABS] = {"MAIN", "FX", "FX2", "SETUP", "MORE"};
    for (int i = 0; i < NTABS && P->mode != M_SMPLR; i++) {
        int on = i == 0 ? P->mode == M_MAIN : i == 1 ? P->mode == M_FX && FX_GROUP < 2 : i == 2 ? P->mode == M_FX && FX_GROUP >= 2 :
                 i == 3 ? P->mode == M_SETUP : P->mode == M_MORE;
        frame(3 + i * (TAB_W + 2), L->foot_y + 2, TAB_W, FOOT - 3, on ? C_CYAN : C_RAIL, 1);
        text_c(3 + i * (TAB_W + 2), L->foot_y + 4, TAB_W, tab[i], on ? C_CYAN : C_GREY, 1);
    }
    int x0 = 3 + NTABS * (TAB_W + 2) + 4;
    if (P->mode == M_SMPLR) {                                     /* SAMPLR is a page of its own: SONG opens it, MIX goes to the Looper; the footer picks the sheet */
        for (int i = 0; i < SM_SHEETS; i++) {
            int on = SM_SHEET == i;
            frame(3 + 60 * i, L->foot_y + 2, 58, FOOT - 3, on ? C_CYAN : C_RAIL, 1);
            text_c(3 + 60 * i, L->foot_y + 4, 58, sm_sheet_name[i], on ? C_CYAN : C_GREY, 1);
        }
        x0 = P->w;
    }
    frame(P->w - 58, L->foot_y + 2, 54, FOOT - 3, C_RAIL, 1);
    text_c(P->w - 58, L->foot_y + 4, 54, "STOCK FX", C_LIGHT, 1);
    if (looper_paused())
        text(x0, L->foot_y + 4, "PAUSE", C_RED, 1);
    else if (P->info_on)
        text(x0, L->foot_y + 4, "SHIFT", C_RED, 1);
}

/* ---- the SAMPLR page: the sample's waveform on top (touch it), below it two rows of big buttons that depend on the sheet picked in the footer:
 * PLAY (the mode and its switches), SAMPLE (which sample, transpose), GESTURE (the take recorder). */
#define SM_W 300                                                  /* waveform: SM_COLS columns of 2 px */
#define SM_BH 28
static void sm_geom(int *wx, int *wy, int *wh, int *ty, int *r1)
{
    struct lay L;
    layout(0, &L);
    *wx = (P->w - SM_W) / 2;
    *ty = TOPBAR + 3;                                             /* the strip above the sample: transpose and the take buttons, always there */
    *wy = *ty + SM_BH + 7;                                        /* (the layer pips and the loop's position fit in the gap) */
    *r1 = L.foot_y - SM_BH - 3;                                   /* the row below it: what the footer's sheet shows */
    *wh = *r1 - 4 - *wy;
}

static void sm_button(int x, int by, int w, const char *s, int on)
{
    frame(x, by, w, SM_BH, on ? C_CYAN : C_RAIL, 1);
    if (on)
        frame(x + 1, by + 1, w - 2, SM_BH - 2, C_CYAN, 1);
    text_c(x, by + (SM_BH - 8) / 2, w, s, on ? C_WHITE : C_LIGHT, 1);
}

static const uint8_t pool_colour[8] = {0x0b, 0x1a, 0x20, 0x06, 0x0b, 0x1a, 0x20, 0x06};   /* each latched loop has its own colour: slice and playhead */
static const char *const sm_mode_name[SM_MODES] = {"SLICE", "TAPE", "ARP", "GRAIN", "LOOP"};
static const char *const sm_q_name[4] = {"Q OFF", "Q 1/4", "Q 1/8", "Q 1/16"};

static void draw_sm_top(struct sm *s, const char *info)
{
    box(1, 1, P->w - 2, TOPBAR, C_BG);
    text(6, 4, info, C_CYAN, 1);
    char b[24], *q = b;                                           /* C: the whole audio task (average / peak %), S: SAMPLR's share, V voices, G grains */
    *q++ = 'C';
    q = put_uint(q, (*(volatile uint16_t *)0x2405ffe2u + 5u) / 10u);
    *q++ = '/';
    q = put_uint(q, (*(volatile uint16_t *)0x2405ffe4u + 5u) / 10u);
    *q++ = ' ';
    *q++ = 'S';
    q = put_uint(q, (s->t_avg_shown + 5u) / 10u);
    *q++ = ' ';
    *q++ = 'V';
    q = put_uint(q, s->n_voices);
    *q++ = ' ';
    *q++ = 'G';
    q = put_uint(q, s->n_grains);
    *q = 0;
    int x = P->w - 6 - 6 * (int)(q - b);
    text(x, 4, b, C_GREY, 1);
    const char *st = 0;
    int col = C_CYAN;
    if (s->g_rec >= 0) {
        st = "REC";
        col = C_RED;
    } else if (s->g_armed) {
        st = "ARMED";
        col = C_YELLOW;
    } else if (s->g_run) {
        st = "LOOP";
    }
    if (st)
        text(x - 6 * 7, 4, st, col, 1);
}

static void draw_samplr(void)
{
    struct sm *s = samplr();
    int wx, wy, wh, ty, r1;
    sm_geom(&wx, &wy, &wh, &ty, &r1);
    box(1, ty - 1, P->w - 2, r1 - ty + SM_BH + 4, C_BG);
    if (!s) {
        box(1, 1, P->w - 2, TOPBAR, C_BG);
        text(6, 4, "SAMPLR: LOOPER MEMORY NOT READY", C_RED, 1);
        return;
    }
    char nm[40], info[40];
    samplr_refresh(looper_ticks());
    samplr_name(nm, 38);
    samplr_info(info);
    draw_sm_top(s, info);
    int cy = wy + wh / 2;
    frame(wx - 1, wy - 1, SM_W + 2, wh + 2, C_RAIL, 1);
    if (s->id < 0) {
        text_c(wx, cy - 4, SM_W, s->npads ? "SAMPLE NOT READY" : "NO SAMPLES ON THE PADS", C_GREY, 1);
        if (!s->npads) {
            char b[16], *q = b;
            q = put_uint(q, s->dbg[0]);
            *q++ = ' ';
            q = put_uint(q, s->dbg[1]);
            *q++ = ' ';
            q = put_uint(q, s->dbg[2]);
            *q = 0;
            text_c(wx, cy + 8, SM_W, b, C_GREY, 1);
        }
    } else {
        int half = wh / 2 - 2, sl_c = 0;
        for (int c = 0; c < SM_COLS; c++) {
            int hi = s->ov[1][c] * half / 127, lo = -s->ov[0][c] * half / 127;
            int32_t fpos = (s->len / SM_COLS) * c;
            while (s->mode == SM_SLICER && sl_c < s->nslice - 1 && fpos >= s->cut[sl_c + 1])
                sl_c++;
            int slice = s->mode == SM_SLICER ? sl_c : -1, col = C_LIGHT;
            for (int f = 0; f < SM_NV; f++)
                if (slice >= 0 && (s->v[f].on || s->v[f].env > 0.f) && s->v[f].slice == slice && s->v[f].vmode == SM_SLICER)
                    col = f < SM_VOICES ? track_colour[f] : f >= SM_LTBASE && f < SM_LTBASE + SM_LATV ? pool_colour[f - SM_LTBASE] : col;
            if (s->mode == SM_LOOP && (fpos < s->lp_a || fpos >= s->lp_b))
                col = C_GREY;                                         /* outside the loop window */
            box(wx + 2 * c, cy - hi, 2, hi + lo + 1, s->ofill[c] ? col : C_DARK);
        }
        if (s->mode == SM_LOOP)
            for (int e = 0; e < 2; e++) {
                int32_t fp = e ? s->lp_b : s->lp_a;
                int x = fp / (s->len / SM_W + 1);
                x = x >= SM_W - 1 ? SM_W - 2 : x;
                vline(wx + x, wy, wh, C_CYAN);
                box(wx + x - (e ? 3 : 0), wy, 4, 7, C_CYAN);          /* the ends: grab one in the strip along the top */
            }
        if (s->mode == SM_SLICER)
            for (int i = 1; i < s->nslice; i++) {
                int x = s->cut[i] / (s->len / SM_W + 1);
                x = x >= SM_W - 1 ? SM_W - 2 : x;
                vline(wx + x, wy, wh, C_GREY);
                box(wx + x - 1, wy, 3, 7, C_YELLOW);                  /* the handle: grab it in the strip along the top */
            }
        if (s->mode == SM_ARP)
            for (int i = 0; i < SM_SPOTS; i++) {
                if (!s->spot[i].used)
                    continue;
                int x = s->spot[i].pos / (s->len / SM_W + 1);
                x = x >= SM_W - 1 ? SM_W - 2 : x;
                int col = s->spot[i].owner < SM_VOICES ? track_colour[s->spot[i].owner] : C_WHITE;
                box(wx + x, wy, s->a_last == i ? 3 : 1, wh, col);
            }
        for (int f = SM_LTBASE; f < SM_LTBASE + SM_LATV; f++) {      /* latched slice loops */
            struct smvoice *v = &s->v[f];
            if (v->on && v->vmode == SM_SLICER)
                vline(wx + (v->ipos / (s->len / SM_W + 1) >= SM_W ? SM_W - 1 : v->ipos / (s->len / SM_W + 1)), wy, wh, pool_colour[f - SM_LTBASE]);
        }
        for (int f = 0; f < SM_VOICES; f++) {
            struct smvoice *v = &s->v[f];
            int x = -1;
            if (s->mode == SM_GRAIN && v->g_on)
                x = v->g_centre / (s->len / SM_W + 1);
            else if (s->mode != SM_ARP && s->mode != SM_GRAIN && (v->on || v->env > 0.f))
                x = v->ipos / (s->len / SM_W + 1);
            if (x >= 0) {
                x = x >= SM_W ? SM_W - 1 : x;
                vline(wx + x, wy, wh, s->mode == SM_GRAIN ? track_colour[f] : C_WHITE);
                if (s->mode == SM_GRAIN) {                        /* the grain size, as a bracket around the centre */
                    int hw = v->g_size / (s->len / SM_W + 1) / 2;
                    hline(wx + (x - hw < 0 ? 0 : x - hw), wy + 2, (x + hw >= SM_W ? SM_W - 1 : x + hw) - (x - hw < 0 ? 0 : x - hw) + 1, track_colour[f]);
                }
            }
        }
        {                                                         /* left: this track's output level; right: its volume (the first encoder) */
            int h = (int)(s->peak * (float)wh);
            h = h > wh ? wh : h;
            box(2, wy, 5, wh, C_DARK);
            box(2, wy + wh - h, 5, h, s->peak > .95f ? C_RED : C_CYAN);
            int vh = (int)(s->vol * .5f * (float)wh);
            vh = vh > wh ? wh : vh;
            box(P->w - 7, wy, 5, wh, C_DARK);
            box(P->w - 7, wy + wh - vh, 5, vh, C_YELLOW);
        }
        if (!s->ov_ok) {                                          /* a streamed sample still loading */
            char b[12], *q = b;
            q = put_uint(q, s->filled * 100u / SM_COLS);
            *q++ = '%';
            *q = 0;
            text(wx + 6, wy + 12, b, C_YELLOW, 1);
        }
    }
    {                                                             /* the strip above the sample: always the six tracks; then pitch, REV, REC, PLAY (or, on the SMPL sheet, the five modes) */
        int nt = samplr_tracks();
        for (int i = 0; i < SM_TRACKS; i++) {
            int x = 3 + 18 * i, inf = i < nt ? samplr_track_info(i) : 0;
            char b[2] = {(char)('1' + i), 0};
            if (i >= nt) {
                frame(x, ty, 16, SM_BH, C_DARK, 1);
                continue;
            }
            frame(x, ty, 16, SM_BH, s->tno == i ? C_CYAN : C_RAIL, 1);
            if (s->tno == i)
                frame(x + 1, ty + 1, 14, SM_BH - 2, C_CYAN, 1);
            text_c(x, ty + (SM_BH - 8) / 2, 16, b, s->tno == i ? C_WHITE : C_LIGHT, 1);
            if (inf & 16)
                box(x + 3, ty + 3, 3, 3, C_YELLOW);                 /* sounding */
            if (inf & 2)
                box(x + 10, ty + 3, 3, 3, C_CYAN);                  /* a loop runs */
            if (inf & 12)
                box(x + 6, ty + SM_BH - 6, 4, 3, C_RED);            /* recording / armed */
        }
        int rec = s->g_rec >= 0, arm = s->g_armed != 0, run = s->g_run;
        if (SM_SHEET == SMS_SMPL) {
            for (int i = 0; i < SM_MODES; i++)
                sm_button(111 + 40 * i, ty, 38, sm_mode_name[i], s->mode == i);
        } else {
            sm_button(113, ty, 28, "-12", 0);
            char b[8], *q = b;
            int tv = s->trans;
            *q++ = tv < 0 ? '-' : '+';
            q = put_uint(q, (unsigned)(tv < 0 ? -tv : tv));
            *q = 0;
            sm_button(143, ty, 36, b, tv != 0);                     /* (drag it sideways for single semitones, tap for 0) */
            sm_button(181, ty, 28, "+12", 0);
            sm_button(211, ty, 28, "REV", s->rev);
            frame(241, ty, 32, SM_BH, rec ? C_RED : arm ? C_YELLOW : C_RAIL, 1);
            if (rec || arm)
                frame(242, ty + 1, 30, SM_BH - 2, rec ? C_RED : C_YELLOW, 1);
            text_c(241, ty + (SM_BH - 8) / 2, 32, "REC", rec ? C_RED : arm ? C_YELLOW : C_LIGHT, 1);
            sm_button(275, ty, 34, run || arm ? "STOP" : "PLAY", run);
            for (int L = 0; L < SM_LAYERS; L++) {                  /* the three layers: grey empty, cyan recorded, red recording */
                int recL = s->g_rec == L, have = L < s->g_layers;
                box(241 + 23 * L, ty + SM_BH + 1, 21, 2, recL ? C_RED : have ? C_CYAN : C_DARK);
            }
            if (run && s->g_len > 0) {                              /* the loop's position */
                int w = (int)((uint32_t)(s->g_pos < 0 ? 0 : s->g_pos) / ((uint32_t)s->g_len / 68u + 1u));
                box(241, ty + SM_BH + 4, w > 68 ? 68 : w, 2, rec ? C_RED : C_CYAN);
            }
        }
    }
    int sh = SM_SHEET < SM_SHEETS ? SM_SHEET : 0;
    if (sh == 0) {                                                /* PLAY: the current mode's switches */
        const char *lab[7] = {0, 0, 0, 0, 0, 0, 0};
        int on[7] = {0, 0, 0, 0, 0, 0, 0};
        lab[2] = "LATCH";
        on[2] = (s->latchm >> s->mode) & 1;
        if (s->mode == SM_SLICER) {
            lab[0] = sm_q_name[s->qi & 3];
            on[0] = s->qi != 0;
            lab[1] = s->loopm ? "LOOP" : s->gate ? "GATE" : "ONE";
            on[1] = s->loopm;
            lab[3] = "AUTO";
            lab[4] = "YP";
            on[4] = (s->ypit >> s->mode) & 1;
            lab[5] = s->seqm == 0 ? "SEQ" : s->seqm == 1 ? "SEQ NAT" : "SEQ GRID";
            on[5] = s->seqm != 0;
            lab[6] = samplr_pat_name(s->pat);
        } else if (s->mode == SM_ARP) {
            lab[0] = "SNAP";
            on[0] = s->qi != 0;
            lab[3] = samplr_pat_name(s->pat);
            lab[4] = "YP";
            on[4] = (s->ypit >> s->mode) & 1;
        } else if (s->mode == SM_LOOP) {
            lab[0] = sm_q_name[s->qi & 3];
            on[0] = s->qi != 0;
            lab[3] = "RESET";
            lab[4] = "YP";
            on[4] = (s->ypit >> s->mode) & 1;
        } else if (s->mode == SM_GRAIN) {
            static const char *const sz_name[3] = {"SIZE 1", "SIZE 1/4", "SIZE 1/16"};
            static const char *const dry_name[4] = {"DRY OFF", "DRY 25", "DRY 50", "DRY 100"};
            lab[0] = s->gfree ? "FREE" : "SYNC";
            lab[1] = samplr_cont_name(s->g_cont);
            lab[3] = s->g_warpmode ? "WARP" : "RND";
            lab[4] = samplr_ppat_name(s->g_ppat);
            on[4] = s->g_ppat != 0;
            lab[5] = sz_name[s->g_sz % 3];
            on[5] = s->g_sz != 0;
            lab[6] = dry_name[s->g_dry & 3];
            on[6] = s->g_dry != 0;
        }
        for (int i = 0; i < 7; i++)
            if (lab[i])
                sm_button(3 + 44 * i, r1, 42, lab[i], on[i]);
    } else if (sh == SMS_REC) {                                   /* REC: the take - UNDO, CLR, LEN, the layers, PANIC */
        sm_button(3, r1, 46, "UNDO", 0);
        sm_button(51, r1, 40, "CLR", 0);
        char b[8], *q = b;
        q = put_uint(q, s->g_bars);
        *q++ = ' ';
        *q++ = 'B';
        *q++ = 'A';
        *q++ = 'R';
        if (s->g_bars > 1)
            *q++ = 'S';
        *q = 0;
        sm_button(93, r1, 58, b, s->g_layers == 0 && !s->g_run);
        for (int L = 0; L < SM_LAYERS; L++) {
            int recL = s->g_rec == L, have = L < s->g_layers;
            int col = recL ? C_RED : have ? C_CYAN : C_RAIL;
            frame(155 + 38 * L, r1, 36, SM_BH, col, 1);
            char d[3] = {'L', (char)('1' + L), 0};
            text_c(155 + 38 * L, r1 + (SM_BH - 8) / 2, 36, d, recL ? C_RED : have ? C_CYAN : C_GREY, 1);
        }
        sm_button(271, r1, 40, "PANIC", 0);                        /* silences every track: loops, latches, clouds, sequences */
    } else if (sh == SMS_FX) {                                    /* FX: filter, resonance, delay and reverb send of this track (the four encoders turn them, pink 1-4), attack and release */
        static const char *const fxn[6] = {"FILT", "RES", "DLY", "REV", "A", "R"};
        for (int i = 0; i < 6; i++) {
            int x0 = 3 + 51 * i;
            frame(x0, r1, 49, SM_BH, C_RAIL, 1);
            float fr = i < 4 ? samplr_fx_frac(i) : (float)(i == 4 ? s->atk : s->rel) * (1.f / 96.f);
            if (i == 0) {
                int w = (int)((fr - .5f) * 44.f);
                box(w < 0 ? x0 + 24 + w : x0 + 24, r1 + SM_BH - 6, w < 0 ? -w : w, 4, C_CYAN);
                vline(x0 + 24, r1 + SM_BH - 8, 8, C_GREY);
            } else {
                box(x0 + 2, r1 + SM_BH - 6, (int)(fr * 45.f), 4, C_CYAN);
            }
            char b[16], *q = b;
            for (const char *z = fxn[i]; *z;)
                *q++ = *z++;
            *q++ = ' ';
            if (i < 4)
                samplr_fx_text(i, q);
            else
                samplr_env_text(i - 4, q);
            text_c(x0, r1 + 4, 49, b, C_LIGHT, 1);
            if (i < 4) {
                char dg[2] = {(char)('1' + i), 0};
                text(x0 + 3, r1 + 3, dg, C_PINK, 1);
            }
        }
    } else {                                                      /* SMPL: which sample, the interpolation (the modes are in the strip above) */
        static const char *const iq_name[3] = {"HIGHQ", "HIGHQ", "LOWP"};
        sm_button(3, r1, 40, "<", 0);
        frame(47, r1, 178, SM_BH, C_RAIL, 1);
        text_c(47, r1 + (SM_BH - 8) / 2, 178, nm[0] ? nm : "-", C_LIGHT, 1);
        sm_button(229, r1, 40, ">", 0);
        sm_button(273, r1, 38, iq_name[s->iq % 3], 0);
    }
}

/* A finger lifts: 1 if it was on the pitch box (a tap there is back to 0). */
static int sm_up(int id)
{
    struct sm *s = samplr();
    int k = id & 3;
    if (!s || s->dmoved[k] < 2)
        return 0;
    if (s->dmoved[k] == 2)
        samplr_trans(0);
    s->dmoved[k] = 0;
    P->sig = 0;
    return 1;
}

/* A touch on the SAMPLR page above the footer. */
static int sm_touch(int kind, int id, int x, int d)
{
    int wx, wy, wh, ty, r1;
    sm_geom(&wx, &wy, &wh, &ty, &r1);
    struct sm *s = samplr();
    if (s && s->dmoved[id & 3] >= 2) {                            /* a finger on the pitch box: sideways = semitones, a tap = back to 0 (the lift comes through sm_up) */
        int k = id & 3;
        if (kind == 1) {
            int st = (x - s->fx0[k]) / 8;
            if (st) {
                samplr_trans(st);
                s->fx0[k] = (int16_t)(s->fx0[k] + st * 8);
                s->dmoved[k] = 3;
            }
        }
        P->sig = 0;
        return 1;
    }
    if (d >= ty - 2 && d < wy - 2) {                              /* the strip above the sample */
        if (kind != 0)
            return 1;
        if (x >= 3 && x < 111 && (x - 3) % 18 < 16 && (x - 3) / 18 < samplr_tracks()) {
            samplr_track((x - 3) / 18);                           /* (the one that was shown goes on playing) */
            P->entered = 0;
        } else if (SM_SHEET == SMS_SMPL) {
            if (x >= 111 && x < 311 && (x - 111) % 40 < 38) {
                samplr_set_mode((x - 111) / 40);
                SM_SHEET = 0;                                     /* mode chosen: back to playing */
                P->entered = 0;
            }
        } else if (x >= 113 && x < 141)
            samplr_trans(-12);
        else if (x >= 143 && x < 179 && s) {
            s->dmoved[id & 3] = 2;
            s->fx0[id & 3] = (int16_t)x;
        } else if (x >= 181 && x < 209)
            samplr_trans(12);
        else if (x >= 211 && x < 239)
            samplr_cycle(11);
        else if (x >= 241 && x < 273)
            samplr_gest(0);
        else if (x >= 275 && x < 309)
            samplr_gest(1);
        P->sig = 0;
        return 1;
    }
    if (d >= r1) {
        int sh = SM_SHEET < SM_SHEETS ? SM_SHEET : 0;
        if (sh == SMS_FX && kind != 2 && s) {                     /* the FX bars (and attack / release) follow the finger */
            int i = (x - 3) / 51, v = x - 3 - 51 * i - 2;
            v = v < 0 ? 0 : v > 45 ? 45 : v;
            if (x >= 3 && i < 6 && (x - 3) % 51 < 49) {
                if (i < 4)
                    samplr_fx_set(i, v * 1023 / 45);
                else
                    samplr_set_env(i - 4, v * 96 / 45);
                P->sig = 0;
            }
            return 1;
        }
        if (kind != 0)
            return 1;
        if (sh == 0) {
            int i = (x - 3) / 44;
            if (x >= 3 && i < 7 && (x - 3) % 44 < 42 && s) {
                int m = s->mode;
                if (i == 0 && m != SM_TAPE)
                    samplr_cycle(0);
                else if (i == 1 && m == SM_SLICER)
                    samplr_toggle_gate();
                else if (i == 1 && m == SM_GRAIN)
                    samplr_cycle(5);
                else if (i == 2)
                    samplr_cycle(2);
                else if (i == 3)
                    samplr_cycle(m == SM_GRAIN ? 6 : m == SM_SLICER ? 4 : m == SM_ARP ? 1 : m == SM_LOOP ? 13 : -1);
                else if (i == 4)
                    samplr_cycle(m == SM_GRAIN ? 7 : m == SM_TAPE ? -1 : 3);
                else if (i == 5 && m == SM_GRAIN)
                    samplr_cycle(8);
                else if (i == 5 && m == SM_SLICER)
                    samplr_cycle(12);
                else if (i == 6 && m == SM_GRAIN)
                    samplr_cycle(9);
                else if (i == 6 && m == SM_SLICER)
                    samplr_cycle(1);
            }
        } else if (sh == SMS_REC) {
            if (x >= 3 && x < 49)
                samplr_gest(2);
            else if (x >= 51 && x < 91)
                samplr_gest(3);
            else if (x >= 93 && x < 151)
                samplr_gest(4);
            else if (x >= 271)
                samplr_stop_all();
        } else {
            if (x < 45)
                samplr_select(-1);
            else if (x >= 227 && x < 271)
                samplr_select(1);
            else if (x >= 273)
                samplr_cycle(10);
        }
        P->sig = 0;
        return 1;
    }
    int fx = (x - wx) * 1024 / SM_W, fy = (d - wy) * 1024 / (wh > 0 ? wh : 1);
    if (kind == 0 && (d < wy || x < wx - 2 || x > wx + SM_W + 2))
        return 1;
    samplr_touch(kind, id & 3, fx, fy);
    P->sig = 0;
    return 1;
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
              (uint32_t)(P->clear_arm == 1 && looper_ticks() - P->clear_t < CLEAR_WAIT) << 29 | (uint32_t)(P->clear_arm == 2 && looper_ticks() - P->clear_t < CLEAR_SHOW) << 28)) * 16777619u;
    for (int o = 0; o < LOOPER_OPTS; o++)
        h = (h ^ (uint32_t)(looper_get_opt(o) * 1000.f + .5f)) * 16777619u;
    return h ^ looper_len() ^ (P->mode == M_SMPLR ? samplr_sig() : 0u);
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
    for (int t = 0; t < LOOPER_TRACKS && P->mode != M_SMPLR; t++) {
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
    else if (P->mode == M_SMPLR)
        draw_samplr();
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
    if ((P->mode == M_SMPLR && (looper_ticks() & 7)) ? ++P->force >= 40 : (signature() != P->sig || ++P->force >= 40)) {
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
    samplr_leave();
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
    if (P->_r6[0]) {                                              /* opened by MIX (MAIN) or SONG (SMPLR) */
        tab_select(P->_r6[0] - 1);
        P->_r6[0] = 0;
    }
    P->entered = 0;
    P->pressed = P_NONE;
    P->info_on = 0;
    P->sig = 0;
    P->force = 0;
}

/* The next time the page is shown it opens on this tab (0 MAIN ... 5 SMPLR). */
void looper_page_want(int tab)
{
    P->_r6[0] = (uint8_t)(tab + 1);
}

int looper_page_tab(void)
{
    return P->mode == M_SMPLR ? 5 : P->mode;
}

/* Switch the tab of the page that is up. */
void looper_page_goto(uint8_t *view, int tab)
{
    tab_select(tab);
    P->info_on = 0;
    P->entered = 0;
    P->sig = 0;
    dirty_now(view);
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
static void probe(int id, int kind)                               /* kind 0 down, 1 move, 2 up; id: the third argument of the touch calls */
{
    if (kind == 0) {                                              /* the ids of the last four touches, newest in the low half-word */
        P->tw2 = (int32_t)(((uint32_t)P->tw2 << 16) | ((uint32_t)P->tw3 >> 16));
        P->tw3 = (int32_t)(((uint32_t)P->tw3 << 16) | ((uint32_t)id & 0xffffu));
    }
    if (kind == 0 && P->tcur < 255)
        P->tcur++;
    else if (kind == 2 && P->tcur)
        P->tcur--;
    if (P->tcur > P->tmax)
        P->tmax = P->tcur;
}

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

static int looper_info_of(int t, int p)                           /* REV (6) / HALF (7) of track t: on? */
{
    struct looper_info k;
    looper_track(t, &k);
    return (p == 6 ? k.reversed : k.half) != 0;
}

static float fx_of(int t, int param)
{
    return looper_get_param(t, param);
}

static void set_fx(int t, int param, float v)
{
    looper_set_param(t, param, v);
}

void looper_page_down(uint8_t *view, const int *pt, int id)
{
    int x, d, t = 0;
    float v = 0.f;
    probe(id, 0);
    P->pressed = P_NONE;
    to_page(pt, &x, &d);
    if (P->mode == M_SMPLR) {
        struct lay L;
        layout(0, &L);
        if (d < L.foot_y) {
            sm_touch(0, id, x, d);
            dirty_now(view);
            return;
        }
    }
    int zone = looper_page_hit(x, d, &t, &v);
    if (zone == Z_NONE)
        return;
    P->track = (uint8_t)t;
    int was_sel = P->sel == t;
    if (zone != Z_TAB && zone != Z_OPTC && zone != Z_SLIDER && zone != Z_CLEAR)
        P->sel = (uint8_t)t;                                      /* touching a track's column selects it */
    if (P->info_on)
        info_touch();
    if (zone == Z_TAB && t >= 10) {                               /* a sheet of the SAMPLR page */
        SM_SHEET = (uint16_t)(t - 10);
        P->entered = 0;
        P->sig = 0;
    } else if (zone == Z_TAB) {
        tab_select(t);
        P->info_on = 0;
        P->entered = 0;                                           /* repaint the whole page for the new tab */
        P->sig = 0;
    } else if (zone == Z_OPTC) {
        if (t >= 95 && t <= 99) {
            int slot = 100 - t;                                   /* LEARN <button>: the next press of it is remembered */
            P->learn = P->learn == slot ? 0 : (uint8_t)slot;
        } else if (t == 94) {
            stock_fx();
#ifdef BANK2
        } else if (t == 93) {                                     /* size test: read back the pattern past the old end of the cave */
            const volatile uint32_t *q = (const volatile uint32_t *)0x08100000u;
            uint32_t bad = 0;
            for (uint32_t i = 0; i < 16384u; i++)
                bad += q[i] != i * 2654435761u;
            P->dropped = bad ? 2u + bad : 1u;
            P->entered = 0;
#endif
        } else {
            looper_set_opt(t, v);
            P->entered = 0;                                       /* e.g. FULL SCREEN: repaint the whole background */
        }
    } else if (zone == Z_SLIDER) {
        looper_set_opt(t, v);
        P->drag_opt = (uint8_t)t;
        P->pressed = P_SLIDER;
    } else if (zone == Z_CLEAR) {
        if (P->clear_arm == 1 && looper_ticks() - P->clear_t < CLEAR_WAIT) {
            P->clear_arm = 2;                                     /* asked twice within 3 s: CLEARED! for a while */
            P->clear_t = looper_ticks();
            looper_clear_all();
        } else {
            P->clear_arm = 1;
            P->clear_t = looper_ticks();
        }
    } else if (zone == Z_FX) {
        int p = (int)(v + .5f);
        int dbl = p != 6 && p != 7 && P->dtap_c == p + 1 && P->dtap_trk == t && looper_ticks() - P->dtap_t < DTAP;
        P->dtap_c = dbl ? 0 : (uint8_t)(p + 1);
        P->dtap_trk = (uint8_t)t;
        P->dtap_t = looper_ticks();
        int was = P->sel == t && FX_GROUP == (p >> 2);
        P->sel = (uint8_t)t;                                      /* a tap moves the one pink square to that block of four */
        set_fx_sel(p >> 2);
        if (dbl) {                                                /* a fast double tap: back to the default */
            if (p == 6 || p == 7) {
                struct looper_info ki;
                looper_track(t, &ki);
                if (fx_value(&ki, p) > .5f)
                    looper_event(t, p == 6 ? LOOPER_EV_REVERSE : LOOPER_EV_HALF);
            } else {
                set_fx(t, fxp(p), fx_default(fxp(p)));
            }
        } else if (p == 6 || p == 7) {
            if (was)                                              /* REV / HALF: a tap on the selected one switches it */
                looper_event(t, p == 6 ? LOOPER_EV_REVERSE : LOOPER_EV_HALF);
        } else {
            P->drag_param = (uint8_t)fxp(p);
            P->drag_y = (int16_t)d;
            P->drag_v0 = (int16_t)(fx_of(t, fxp(p)) * 1000.f);
            P->pressed = P_DIAL;
        }
    } else if (zone == Z_SEL) {
        P->sel = (uint8_t)t;
    } else if (zone == Z_UNDO) {
        looper_event(t, LOOPER_EV_UNDO);
    } else if (zone == Z_HALF) {
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

void looper_page_move(uint8_t *view, const int *pt, int id)
{
    probe(id, 1);
    if (P->mode == M_SMPLR) {
        int x, d;
        to_page(pt, &x, &d);
        sm_touch(1, id, x, d);
        dirty_now(view);
        return;
    }
    if (P->pressed != P_FADER && P->pressed != P_DIAL && P->pressed != P_SLIDER)
        return;
    int x, d;
    to_page(pt, &x, &d);
    struct lay L;
    layout(P->track, &L);
    if (P->pressed == P_FADER) {
        looper_set_level(P->track, gain_of((float)(L.bars_y + L.bars_h - d) / (float)L.bars_h));
    } else if (P->pressed == P_DIAL) {                            /* 80 px of travel for the full range */
        float span = fx_bipolar(P->drag_param) ? 2.f : 1.f;
        set_fx(P->track, P->drag_param, (float)P->drag_v0 * .001f + (float)(P->drag_y - d) * span / 80.f);
    } else {
        float v = (float)(x - OPT_X) / (float)SLIDER_W;
        looper_set_opt(P->drag_opt, v < 0.f ? 0.f : v > 1.f ? 1.f : v);
    }
    dirty_now(view);
}

void looper_page_up(uint8_t *view, const int *pt, int id)
{
    (void)pt;
    probe(id, 2);
    if (P->mode != M_SMPLR || !sm_up(id))
        samplr_touch(2, id & 3, 0, 0);
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
            if (P->mode == M_SMPLR) {
                struct sm *sm = samplr();
                if (sm && SM_SHEET == SMS_FX)
                    samplr_fx_knob(knob, counts);
                else if (sm)
                    samplr_knob(knob, counts);
            } else if (P->mode == M_MAIN)
                P->sel = (uint8_t)knob;                                   /* altering a track's setting selects it */
            if (P->mode == M_SMPLR) {
            } else if (P->mode == M_SETUP || P->mode == M_MORE) {
                static const uint8_t op_s[4] = {LOOPER_O_GAIN, 0, 0, 0};
                static const uint8_t op_m[4] = {LOOPER_O_DFB, LOOPER_O_DRET, LOOPER_O_RSIZE, LOOPER_O_RRET};
                if (P->mode == M_MORE)
                    looper_set_opt(op_m[knob], looper_get_opt(op_m[knob]) + step);
                else if (knob == 0)
                    looper_set_opt(op_s[0], looper_get_opt(op_s[0]) + step);
            } else if (P->mode == M_FX) {
                int p = FX_GROUP * 4 + knob_slot[knob & 3], tk = P->sel & 3;             /* knob n turns the n-th control of the pink square, on the selected track */
                if (p >= FX_N) {
                } else if (p == 6 || p == 7) {                            /* REV / HALF: turn right = on, left = off */
                    int on = looper_info_of(tk, p);
                    if ((counts > 0) != on)
                        looper_event(tk, p == 6 ? LOOPER_EV_REVERSE : LOOPER_EV_HALF);
                } else {
                    int q = fxp(p);
                    set_fx(tk, q, fx_of(tk, q) + step * (fx_bipolar(q) ? 2.f : 1.f));
                }
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
    if (P->mode == M_FX) {                                         /* FX tab: INFO moves the pink box on by one (all four tracks) */
        if (FX_GROUP & 1) {                                       /* INFO moves the square on: this page's two blocks, then the next track's */
            P->sel = (uint8_t)((P->sel + 1) & 3);
            set_fx_sel(FX_GROUP & 2);
        } else {
            set_fx_sel(FX_GROUP + 1);
        }
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
    if (P->magic == PMAGIC) {                                     /* screen history: the stock pages' ids, read later on MORE */
        uint8_t sc = ((const uint8_t *)app)[0x8ca4];
        if (P->scr_h[0] != sc) {
            for (int i = 7; i > 0; i--)
                P->scr_h[i] = P->scr_h[i - 1];
            P->scr_h[0] = sc;
        }
    }
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
                if (!P->bset[slot] || idx != P->bidx[slot])
                    continue;
                if (id != P->bid[slot]) {
                    if (id >= 0xf4 && id <= 0xf9 && P->bid[slot] >= 0xf4 && P->bid[slot] <= 0xf9)
                        return;                                /* the same button's other message (REC: 244 down, 245 up): taken, not an action */
                    continue;
                }
                if (looper_ticks() - P->btn_t <= 20) {             /* a press and its release can both arrive: one action */
                    if ((slot == B_STOP || slot == B_PLAY) && looper_get_opt(LOOPER_O_HWBTN) > .5f)
                        break;
                    return;
                }
                P->btn_t = looper_ticks();
                if (P->mode == M_SMPLR) {                          /* on the SAMPLR page the buttons are SAMPLR's, never the looper's */
                    if (slot == B_FX)
                        break;                                     /* (FX stays the stock FX button) */
                    if (slot == B_REC)
                        samplr_gest(0);
                    else if (slot == B_BACK)
                        samplr_gest(2);
                    else if (slot == B_STOP)
                        samplr_gest(6);
                    else
                        samplr_gest(5);
                    P->sig = 0;
                    if ((slot == B_STOP || slot == B_PLAY) && looper_get_opt(LOOPER_O_HWBTN) > .5f)
                        break;
                    return;
                }
                switch (slot) {
                case B_FX:
                    if (P->info_on)                                /* INFO + FX: the Blackbox's own FX page */
                        stock_fx();
                    else if (P->mode != M_FX)                      /* the FX button: MAIN -> FX -> FX2 -> MAIN */
                        tab_select(1);
                    else if (FX_GROUP < 2)
                        tab_select(2);
                    else
                        tab_select(0);
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
