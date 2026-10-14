/* The LIRA page (mode M_LIRA of the Looper page), included by looper_page.c where its drawing helpers are in scope.
 * Opened by the PAD button (second press on the pad screen). The engine is lira_pad.c's page instance, which the looper's
 * output stage runs: no pad or sample needed.
 *
 * One 8 x 38 px grid carries everything. At the bottom, always: the eight voices in two groups of four (a TUNE knob each, in its
 * pair's colour, and a big plate below it: tap = on / off, the top-right switch makes the plates HOLD). The eight hardware
 * buttons are the same plates. Above them one of three tabs (footer), each a row of four panels of four controls:
 *   VOICES   one panel per voice pair: MOD, SHARP, FM source, TOUCH
 *   SOUND    DELAY (time 1, 2, feedback, mix), MOD (mod 1, 2, LFO A, B), PITCH (hold 1-4, 5-8, pitch 1-4, 5-8), LFO (shape, combine, link, delay source)
 *   MIX      LEVEL 1-4, LEVEL 5-8 (a fader per voice), OUTPUT (drive, dist, volume, ext in)
 *   CHORD    TONAL SPACE (space, root, register, span), VOICING (chord on, tuning, shuffle, rotate), the notes of the voices
 * The four encoders turn the four controls of the active group (white frame): the strip's voices 1-4 or 5-8, or a panel. A touch on
 * a control makes its group the active one; BACK flips between voices 1-4 and 5-8. Inside a panel the encoders sit like the
 * controls: top-left, top-right, bottom-left, bottom-right.
 */
#include "lira_chord.h"

#define LU_LEVEL  120
#define LU_MAXS   32
#define LU_COL    38                                              /* the grid: eight columns of 38 px from x = 5 */
#define LU_X0     5
#define LU_PANEL  16                                              /* the panels' top (d) and height */
#define LU_PANH   91
#define LU_STRIP  110                                             /* the voice groups' top */
#define LU_PLATE  157
#define LU_CARD   0x19                                            /* the card behind a control */
#define LU_LINE   0x10                                            /* hairlines, frames */
#define LU_DIM    0x16                                            /* labels */
#define LU_ACC1   0x1a                                            /* the first voice pair's colour (violet), and the third's */
#define LU_ACC2   0x1c                                            /* the second pair's colour (seafoam), and the fourth's */
#define LU_ACC1D  0x1d                                            /* their dark versions: a plate that is on */
#define LU_ACC2D  0x1f
#define LU_SEL    0x20                                            /* the encoders' group (orange): its frame and labels */

/* The page's colours. The firmware's palette is a RAM table (0x24000000, one ARGB word per index) and a fill turns the index into a
 * colour when it runs, so while the page is drawn these entries are replaced and then put back: the pixels keep the page's colours,
 * everything else the Blackbox draws keeps its own. */
static const struct { uint8_t idx; uint32_t argb; } lu_theme[] = {
    {0x02, 0xff0c0c14},                                           /* background: midnight */
    {0x19, 0xff15151f},                                           /* cards */
    {0x10, 0xff34344a},                                           /* lines */
    {0x09, 0xff767694},                                          /* knob tracks, off plates' numbers */
    {0x16, 0xffb9b9d6},                                           /* labels */
    {0x0f, 0xffffffff},                                           /* highlights, values, pointers: full white */
    {0x1a, 0xff9a82e8},                                           /* violet */
    {0x1c, 0xff6fd1b6},                                           /* seafoam */
    {0x1d, 0xff3b3263},                                           /* dark violet */
    {0x1f, 0xff21493e},                                           /* dark seafoam */
    {0x20, 0xfff09a4a},                                           /* orange: the encoders' group */
};
#define LU_NTHEME ((int)(sizeof lu_theme / sizeof lu_theme[0]))

static void lu_theme_set(uint32_t *save)
{
    volatile uint32_t *pal = (volatile uint32_t *)0x24000000u;
    for (int i = 0; i < LU_NTHEME; i++) {
        save[i] = pal[lu_theme[i].idx];
        pal[lu_theme[i].idx] = lu_theme[i].argb;
    }
}

static void lu_theme_restore(const uint32_t *save)
{
    volatile uint32_t *pal = (volatile uint32_t *)0x24000000u;
    for (int i = 0; i < LU_NTHEME; i++)
        pal[lu_theme[i].idx] = save[i];
}

static int lu_dark(int col)
{
    return col == LU_ACC1 ? LU_ACC1D : LU_ACC2D;
}
enum { LK_KNOB, LK_STEP, LK_BTN, LK_NOTE };

struct lu_spec {
    uint16_t id;
    int16_t x, d, w, h;
    int8_t bank, enc;                                             /* the encoder group it belongs to, and which encoder (post-swap: 0 bottom-left, 1 top-left, 2 top-right, 3 bottom-right) */
    uint8_t kind, col, r;
    const char *name;
};
#define LP_VOICES 0
#define LP_SOUND  1
#define LP_MIX    2
#define LP_CHORD  3
static const char *const lu_tabname[4] = {"VOICES", "SOUND", "MIX", "CHORD"};
static const char *const lu_lfo_name[4] = {"SHAPE", "COMBINE", "LINK", "DLY SRC"};
static const char *const lu_lfo_val[4][3] = {{"TRI", "SQR", ""}, {"PROD", "SUM", ""}, {"OFF", "ON", ""}, {"SELF", "OFF", "LFO"}};
static const char *const lu_touch[3] = {"SLOW", "FAST", "EXT"};
static const char *const lu_src[8] = {"SELF", "OFF", "LFO", "EXT", "P1-2", "P3-4", "P5-6", "P7-8"};
static const uint8_t lu_pair_col[4] = {LU_ACC1, LU_ACC2, LU_ACC1, LU_ACC2};
static const int8_t lu_enc_cell[4] = {1, 2, 0, 3};                /* a panel's cells (top-left, top-right, bottom-left, bottom-right) -> encoder */
static const int8_t lu_enc_row[4] = {0, 1, 2, 3};                 /* a row of four voices -> encoder: the left two on the left encoders */

#define LU_SP(i, I, X, D, W, H, B, E, K, C, R, N) o[i] = (struct lu_spec){I, X, D, W, H, B, E, K, C, R, N}

/* the cell c (0..3) of panel p */
static void lu_cell(int p, int c, int *x, int *d)
{
    *x = LU_X0 + p * 2 * LU_COL + (c & 1) * LU_COL;
    *d = LU_PANEL + 12 + (c >> 1) * 38;
}

static int lu_specs(int sel, struct lu_spec *o)
{
    int page = sel & 15, n = 0;
    static const char *const vn[8] = {"1", "2", "3", "4", "5", "6", "7", "8"};
    for (int v = 0; v < 8; v++) {                                 /* the voice knobs, on every page */
        LU_SP(n, v, LU_X0 + v * LU_COL, LU_STRIP + 3, LU_COL, 40, v >> 2, lu_enc_row[v & 3], LK_KNOB, lu_pair_col[v >> 1], 12, vn[v]);
        n++;
    }
    int x, d;
    if (page == LP_VOICES) {
        for (int p = 0; p < 4; p++) {
            lu_cell(p, 0, &x, &d);
            LU_SP(n, 108 + p, x, d, LU_COL, 38, 2 + p, lu_enc_cell[0], LK_KNOB, lu_pair_col[p], 11, "MOD");
            n++;
            lu_cell(p, 1, &x, &d);
            LU_SP(n, 130 + p, x, d, LU_COL, 38, 2 + p, lu_enc_cell[1], LK_KNOB, lu_pair_col[p], 11, "SHARP");
            n++;
            lu_cell(p, 2, &x, &d);
            LU_SP(n, 104 + p, x, d, LU_COL, 38, 2 + p, lu_enc_cell[2], LK_BTN, lu_pair_col[p], 0, "FM");
            n++;
            lu_cell(p, 3, &x, &d);
            LU_SP(n, 100 + p, x, d, LU_COL, 38, 2 + p, lu_enc_cell[3], LK_BTN, lu_pair_col[p], 0, "TOUCH");
            n++;
        }
    } else if (page == LP_SOUND || page == LP_MIX) {
        static const uint16_t ids[2][16] = {{16, 17, 18, 19, 125, 126, 14, 15, 121, 122, 123, 124, 160, 161, 162, 163},
                                            {170, 171, 172, 173, 174, 175, 176, 177, 22, 23, LU_LEVEL, 21, 0, 0, 0, 0}};
        static const char *const nm[2][16] = {{"TIME1", "TIME2", "FDBK", "MIX", "MOD1", "MOD2", "LFO A", "LFO B",
                                               "H 1-4", "H 5-8", "P 1-4", "P 5-8", "SHAPE", "COMBINE", "LINK", "DLY SRC"},
                                              {"LV 1", "LV 2", "LV 3", "LV 4", "LV 5", "LV 6", "LV 7", "LV 8",
                                               "DRIVE", "DIST", "VOL", "EXT", "", "", "", ""}};
        int pg = page == LP_MIX;
        for (int i = 0; i < (pg ? 12 : 16); i++) {
            lu_cell(i >> 2, i & 3, &x, &d);
            LU_SP(n, ids[pg][i], x, d, LU_COL, 38, 2 + (i >> 2), lu_enc_cell[i & 3], pg && i < 8 ? LK_KNOB : !pg && i >= 12 ? LK_BTN : LK_KNOB,
                  pg && i < 8 ? lu_pair_col[i >> 1] : LU_ACC1, 11, nm[pg][i]);
            n++;
        }
    } else {
        static const char *const nm[8] = {"SPACE", "ROOT", "OCT", "SPAN", "CHORD", "TUNE", "SHUFL", "CYCLE"};
        static const uint16_t ids[8] = {140, 141, 142, 143, 150, 153, 151, 152};
        static const uint8_t cellof[8] = {0, 1, 2, 3, 0, 1, 2, 3};
        for (int i = 0; i < 8; i++) {
            lu_cell(i >> 2, cellof[i], &x, &d);
            LU_SP(n, ids[i], x, d, LU_COL, 38, 2 + (i >> 2), lu_enc_cell[cellof[i]], i < 4 ? LK_STEP : LK_BTN, LU_ACC1, 11, nm[i]);
            n++;
        }
        static const uint8_t vcell[8] = {0, 1, 2, 3, 0, 1, 2, 3};
        for (int v = 0; v < 8; v++) {                             /* the notes of the voices, a display */
            lu_cell(2 + (v >> 2), vcell[v], &x, &d);
            LU_SP(n, 200 + v, x, d, LU_COL, 38, 4 + (v >> 2), lu_enc_cell[vcell[v]], LK_NOTE, lu_pair_col[v >> 1], 0, vn[v]);
            n++;
        }
    }
    return n;
}

static void lu_plate_rect(int v, int *x, int *d, int *w, int *h)
{
    struct lay L;
    layout(0, &L);
    *x = LU_X0 + v * LU_COL + 1;
    *d = LU_PLATE;
    *w = LU_COL - 2;
    *h = L.foot_y - 4 - LU_PLATE;
}

static void lu_pct(char **q, int v)
{
    *q = put_uint(*q, (unsigned)((v + 5) / 10));
}

static int lu_stepmax(int id)
{
    return id == 140 ? lira_space_count() - 1 : 3 + (id == 141) * 8;
}

/* the knob position (0..1000) a control shows */
static int lu_value(const struct lu_spec *s)
{
    if (s->kind == LK_STEP)
        return lira_ui_chord_get(s->id) * 1000 / lu_stepmax(s->id);
    return lira_ui_get(s->id);
}

/* A knob like a panel's: a dotted track (the arc table of the looper's dial, scaled), the value arc in the colour, a white
 * pointer and a cap. cx / cy = the centre, r = the radius in pixels. */
static void lu_knob(int cx, int cy, int r, int v, int col)
{
    int n = (v * 27 + 500) / 1000;
    for (int i = 0; i < 28; i += 2)
        box(cx + arc[i][0] * r / 11, cy + arc[i][1] * r / 11, 1, 1, C_GREY);
    for (int i = 0; i <= n; i++)
        box(cx + arc[i][0] * r / 11, cy + arc[i][1] * r / 11, 2, 2, col);
    for (int k = 2; k <= r - 3; k++)
        box(cx + arc[n][0] * k / 11, cy + arc[n][1] * k / 11, 1, 1, C_WHITE);
    box(cx - 1, cy - 1, 3, 3, C_GREY);
}

/* a rounded square, as three boxes */
static void lu_round(int x, int d, int w, int h, int col)
{
    box(x + 2, d, w - 4, h, col);
    box(x, d + 2, w, h - 4, col);
    box(x + 1, d + 1, w - 2, h - 2, col);
}

static void lu_draw_spec(const struct lu_spec *s, int bank, int chord)
{
    int cx = s->x + s->w / 2, hot = s->bank == bank && s->bank >= 0;
    char b[8], *q = b;
    box(s->x + 2, s->d, s->w - 4, s->h, LU_CARD);
    if (s->kind == LK_KNOB || s->kind == LK_STEP) {
        int v = lu_value(s), cy = s->d + s->r + 2;
        lu_knob(cx, cy, s->r, v, chord && s->id < 8 ? C_GREY : s->col);
        if (s->id < 8 && chord) {                                 /* chord mode: the voice's note */
            lira_note_name(lira_ui_note(s->id), lira_ui_flats(), b);
            text_c(s->x, cy + s->r + 3, s->w, b, C_WHITE, 1);
        } else if (s->id < 8) {
            q = put_uint(b, (unsigned)(s->id + 1));
            *q = 0;
            text_c(s->x, cy + s->r + 3, s->w, b, hot ? LU_SEL : LU_DIM, 1);
        } else if (s->kind == LK_STEP && s->id != 140) {          /* a stepped knob shows its setting */
            int k = lira_ui_chord_get(s->id);
            const char *t = b;
            if (s->id == 141) {
                t = lira_root_name(k);
            } else if (s->id == 142) {
                lira_note_name(36 + 12 * k, 0, b);
            } else {
                b[0] = (char)('1' + k);
                b[1] = ' ';
                b[2] = 'O';
                b[3] = 'C';
                b[4] = 'T';
                b[5] = 0;
            }
            text_c(s->x, cy + s->r + 3, s->w, t, hot ? LU_SEL : LU_DIM, 1);
        } else {
            text_c(s->x, cy + s->r + 3, s->w, s->name, hot ? LU_SEL : LU_DIM, 1);
        }
    } else if (s->kind == LK_BTN) {                               /* two lines: what it is, what it is set to */
        const char *st = "";
        int on = 1;
        if (s->id >= 100 && s->id < 104)
            st = lu_touch[lira_ui_get(s->id)], on = lira_ui_get(s->id) != 0;
        else if (s->id >= 104 && s->id < 108)
            st = lu_src[lira_ui_get(s->id)], on = lira_ui_get(s->id) != 1;
        else if (s->id >= 160 && s->id < 164) {
            int k = lira_ui_chord_get(s->id);
            st = lu_lfo_val[s->id - 160][k];
            on = s->id == 163 ? k != 1 : k != 0;
        } else if (s->id == 150)
            st = lira_ui_chord_get(150) ? "ON" : "OFF", on = lira_ui_chord_get(150);
        else if (s->id == 153)
            st = lira_ui_chord_get(153) ? "JUST" : "EQUAL", on = lira_ui_chord_get(153);
        else
            on = 0;
        if (on)
            box(s->x + 3, s->d + 3, s->w - 6, s->h - 6, lu_dark(s->col));
        frame(s->x + 2, s->d + 2, s->w - 4, s->h - 4, on ? s->col : LU_LINE, 1);
        text_c(s->x, s->d + 8, s->w, s->name, hot ? LU_SEL : LU_DIM, 1);
        if (*st)
            text_c(s->x, s->d + 21, s->w, st, on ? C_WHITE : C_GREY, 1);
    } else {                                                      /* a note of a voice */
        lira_note_name(lira_ui_note(s->id - 200), lira_ui_flats(), b);
        {                                                         /* the letter big, the sharp and octave small */
            int rest = 0;
            while (b[1 + rest])
                rest++;
            int x0 = s->x + (s->w - 12 - 6 * rest) / 2;
            char l1[2] = {b[0], 0};
            text(x0, s->d + 6, l1, chord ? C_WHITE : C_GREY, 2);
            text(x0 + 12, s->d + 14, b + 1, chord ? C_WHITE : C_GREY, 1);
        }
        q = put_uint(b, (unsigned)(s->id - 200 + 1));
        *q = 0;
        text_c(s->x, s->d + 26, s->w, b, s->col, 1);
    }
}

static void draw_lira(void)
{
    struct lay L;
    layout(0, &L);
    int sel = lira_ui_sel(-1), page = sel & 15, bank = (sel >> 4) & 7, chord = lira_ui_chord_get(150);
    struct lu_spec sp[LU_MAXS];
    int n = lu_specs(sel, sp);
    char b[40], *q;
    static const char *const ptitle[4][4] = {{"PAIR 1-2", "PAIR 3-4", "PAIR 5-6", "PAIR 7-8"},
                                             {"DELAY", "MOD", "HOLD/PITCH", "LFO"},
                                             {"LEVEL 1-4", "LEVEL 5-8", "OUTPUT", ""},
                                             {"TONAL SPACE", "VOICING", "VOICES 1-4", "VOICES 5-8"}};
    box(1, 1, P->w - 2, TOPBAR, C_BG);
    text(6, 4, "DRONE-8", C_WHITE, 1);
    int last = lira_ui_last(-1);
    q = b;
    *q = 0;
    if (page == LP_CHORD) {                                       /* the tonal space and its root */
        for (const char *t = lira_ui_space_name(); *t;)
            *q++ = *t++;
        *q++ = ' ';
        for (const char *t = lira_root_name(lira_ui_chord_get(141)); *t;)
            *q++ = *t++;
        *q = 0;
    } else {
        for (int i = 0; i < n; i++)
            if (sp[i].id == last && sp[i].kind == LK_KNOB) {
                if (last < 8) {
                    char *t = b;
                    *t++ = 'V';
                    *t++ = (char)('1' + last);
                    q = t;
                } else {
                    for (const char *t = sp[i].name; *t;)
                        *q++ = *t++;
                }
                *q++ = ' ';
                lu_pct(&q, lira_ui_get(last));
                *q = 0;
            }
    }
    if (!*b) {                                                    /* (a diagnostic for now: free SDRAM, this synth's share of the audio task, the task's load, shedding) */
        q = b;
        if (lira_ui_load(2)) {
            for (const char *t = "SHED "; *t;)
                *q++ = *t++;
            q = put_uint(q, (unsigned)lira_ui_load(2));
        } else {
            q = put_uint(q, lira_free_kb());
            *q++ = 'K';
        }
        *q++ = ' ';
        *q++ = 'D';
        q = put_uint(q, (unsigned)lira_ui_load(0));
        *q++ = ' ';
        *q++ = 'C';
        q = put_uint(q, (unsigned)lira_ui_load(1));
        *q = 0;
    }
    text(54, 4, b, *b ? C_WHITE : C_GREY, 1);
    int mom = lira_ui_momentary(0), sine = lira_ui_chord_get(154);
    frame(P->w - 50, 2, 46, TOPBAR - 2, mom ? LU_ACC2 : LU_LINE, 1);
    text_c(P->w - 50, 4, 46, mom ? "HOLD" : "LATCH", LU_DIM, 1);
    int xs = lira_ui_chord_get(156);
    frame(P->w - 150, 2, 46, TOPBAR - 2, xs ? LU_ACC2 : LU_LINE, 1);
    text_c(P->w - 150, 4, 46, xs == 0 ? "X:IN" : xs == 1 ? "X:MIX" : "X:BOTH", xs ? C_WHITE : LU_DIM, 1);
    frame(P->w - 100, 2, 46, TOPBAR - 2, sine ? LU_ACC1 : LU_LINE, 1);
    text_c(P->w - 100, 4, 46, sine ? "SINE" : "TRI", sine ? C_WHITE : LU_DIM, 1);
    box(1, TOPBAR + 2, P->w - 2, L.foot_y - TOPBAR - 3, C_BG);

    for (int p = 0; p < 4; p++) {                                 /* the four panels */
        if (page == LP_MIX && p == 3)
            continue;
        int x = LU_X0 + p * 2 * LU_COL, hot = bank == 2 + p;
        frame(x + 1, LU_PANEL, 2 * LU_COL - 2, LU_PANH, hot ? LU_SEL : LU_LINE, 1);
        text_c(x, LU_PANEL + 4, 2 * LU_COL, ptitle[page][p], hot ? LU_SEL : LU_DIM, 1);
        if (page == LP_VOICES)
            box(x + 6, LU_PANEL + 13, 2 * LU_COL - 12, 1, lu_pair_col[p]);
    }
    for (int g = 0; g < 2; g++) {                                 /* the voice groups */
        int x = LU_X0 + g * 4 * LU_COL, hot = bank == g;
        frame(x + 1, LU_STRIP, 4 * LU_COL - 2, 46, hot ? LU_SEL : LU_LINE, 1);
        for (int k = 0; k < 2; k++)
            box(x + k * 2 * LU_COL + 6, LU_STRIP, 2 * LU_COL - 12, 2, lu_pair_col[g * 2 + k]);
    }
    for (int i = 0; i < n; i++)
        lu_draw_spec(&sp[i], bank, chord);
    for (int v = 0; v < 8; v++) {                                 /* the plates */
        int x, d, w, h, on = lira_ui_key_get(v), col = lu_pair_col[v >> 1];
        lu_plate_rect(v, &x, &d, &w, &h);
        lu_round(x, d, w, h, on ? col : LU_LINE);
        lu_round(x + 2, d + 2, w - 4, h - 4, on ? lu_dark(col) : LU_CARD);
        q = put_uint(b, (unsigned)(v + 1));
        *q = 0;
        text_c(x, d + h / 2 - 8, w, b, on ? C_WHITE : C_GREY, 2);
    }
}

/* the footer: the page tabs (the stock FX button sits at the right, drawn by the caller) */
static void lira_footer(const struct lay *L)
{
    int page = lira_ui_sel(-1) & 15;
    for (int i = 0; i < 4; i++) {
        frame(LU_X0 + 62 * i, L->foot_y + 2, 60, FOOT - 3, page == i ? C_WHITE : LU_LINE, 1);
        text_c(LU_X0 + 62 * i, L->foot_y + 4, 60, lu_tabname[i], page == i ? C_WHITE : C_GREY, 1);
    }
}

static void lira_page_set(int p, int bank)
{
    lira_ui_sel((p & 15) | (bank & 7) << 4);
    P->entered = 0;
    P->sig = 0;
}

static int lira_footer_hit(int x)
{
    int i = (x - LU_X0) / 62;
    if (x < LU_X0 || i > 3 || (x - LU_X0) % 62 >= 60)
        return 0;
    lira_page_set(i, 2);                                          /* a new tab: its first panel is the encoder group */
    return 1;
}

/* BACK: the encoders go to the other four voices (from a panel: to voices 1-4) */
static void lira_back(void)
{
    int sel = lira_ui_sel(-1), bank = (sel >> 4) & 7;
    lira_page_set(sel & 15, bank == 0 ? 1 : 0);
}

static void lu_bank(int bank)
{
    int sel = lira_ui_sel(-1);
    if (((sel >> 4) & 7) != bank) {
        lira_ui_sel((sel & 15) | bank << 4);
        P->entered = 0;
        P->sig = 0;
    }
}

/* a control set to a position (0..1000) */
static void lu_set(const struct lu_spec *s, int v)
{
    v = v < 0 ? 0 : v > 1000 ? 1000 : v;
    if (s->kind == LK_STEP) {
        int mx = lu_stepmax(s->id);
        lira_ui_chord_set(s->id, (v * mx + 500) / 1000);
    } else if (s->kind == LK_KNOB && !(s->id < 8 && lira_ui_chord_get(150))) {
        lira_ui_set(s->id, v);
    }
}

/* a control back to its default (the double tap) */
static void lu_reset(const struct lu_spec *s)
{
    if (s->kind == LK_STEP) {
        static const uint8_t def[4] = {0, 0, 1, 2};
        lira_ui_chord_set(s->id, def[s->id - 140]);
    } else if (s->kind == LK_KNOB && !(s->id < 8 && lira_ui_chord_get(150))) {
        lira_ui_set(s->id, lira_ui_default(s->id));
    }
}

/* tapping a button, or stepping it with an encoder */
static void lu_press(const struct lu_spec *s, int dir)
{
    int id = s->id;
    if (id >= 100 && id < 104)
        lira_ui_set(id, (lira_ui_get(id) + (dir < 0 ? 2 : 1)) % 3);
    else if (id >= 104 && id < 108)
        lira_ui_set(id, (lira_ui_get(id) + (dir < 0 ? 7 : 1)) & 7);
    else if (id >= 160 && id < 164)
        lira_ui_action(10 + id - 160);
    else if (id == 150)
        lira_ui_action(0);
    else if (id == 151)
        lira_ui_action(1);
    else if (id == 152)
        lira_ui_action(2);
    else if (id == 153)
        lira_ui_action(3);
}

/* kind 0 down, 1 move, 2 up; x / d in page pixels. A knob turns by 80 px of travel up or down from where it was touched. */
static void lira_touch(int kind, int id, int x, int d)
{
    int sel = lira_ui_sel(-1);
    struct lu_spec sp[LU_MAXS];
    int n = lu_specs(sel, sp);
    if (kind == 2) {
        if (lira_ui_drag(id, 1, 0, 0, 0, 0) == 1)
            lira_ui_key(lira_ui_drag(id, 2, 0, 0, 0, 0), 0);
        lira_ui_drag(id, 0, 0, 0, 0, 0);
        return;
    }
    if (kind == 1) {
        if (lira_ui_drag(id, 1, 0, 0, 0, 0) == 3) {
            int i = lira_ui_drag(id, 2, 0, 0, 0, 0), dy = lira_ui_drag(id, 3, 0, 0, 0, 0) - d;
            if (dy > 3 || dy < -3)
                lira_ui_tap(0xffff, 0, 0);                        /* a drag is not the first half of a double tap */
            if (i < n && sp[i].id < 8 && lira_ui_chord_get(150))       /* chord mode: the voice's note, 60 semitones over 80 px */
                lira_ui_note_set(sp[i].id, lira_ui_drag(id, 4, 0, 0, 0, 0) + (lira_ui_drag(id, 3, 0, 0, 0, 0) - d) * 60 / 80);
            else if (i < n)
                lu_set(&sp[i], lira_ui_drag(id, 4, 0, 0, 0, 0) + (lira_ui_drag(id, 3, 0, 0, 0, 0) - d) * 1000 / 80);
        }
        return;
    }
    lira_ui_drag(id, 0, 0, 0, 0, 0);
    if (d <= TOPBAR + 1) {
        if (x >= P->w - 50)
            lira_ui_momentary(1);
        else if (x >= P->w - 100)
            lira_ui_action(4);
        else if (x >= P->w - 150)
            lira_ui_action(5);
        return;
    }
    for (int v = 0; v < 8; v++) {                                 /* plates */
        int px, pd, pw, ph;
        lu_plate_rect(v, &px, &pd, &pw, &ph);
        if (x >= px - 1 && x < px + pw + 1 && d >= pd && d < pd + ph + 4) {
            if (lira_ui_momentary(0)) {
                lira_ui_key(v, 1);
                lira_ui_drag(id, 0, 1, v, 0, 0);
            } else {
                lira_ui_key(v, !lira_ui_key_get(v));
            }
            return;
        }
    }
    for (int i = 0; i < n; i++)
        if (x >= sp[i].x && x < sp[i].x + sp[i].w && d >= sp[i].d && d < sp[i].d + sp[i].h && sp[i].kind != LK_NOTE) {
            lu_bank(sp[i].bank);
            lira_ui_last(sp[i].id);
            if (sp[i].kind == LK_BTN)
                lu_press(&sp[i], 1);
            else if (lira_ui_tap(sp[i].id, looper_ticks(), 10))
                lu_reset(&sp[i]);                                 /* a double tap: back to the default */
            else
                lira_ui_drag(id, 0, 3, i, d, sp[i].id < 8 && lira_ui_chord_get(150) ? lira_ui_note(sp[i].id) : lu_value(&sp[i]));
            return;
        }
    for (int p = 0; p < 4; p++)                                   /* the empty part of a panel or voice group: just makes it the encoders' */
        if (x >= LU_X0 + p * 2 * LU_COL && x < LU_X0 + (p + 1) * 2 * LU_COL && d >= LU_PANEL && d < LU_PANEL + LU_PANH) {
            lu_bank(2 + p);
            return;
        }
    if (d >= LU_STRIP && d < LU_STRIP + 46 && x >= LU_X0)
        lu_bank(x < LU_X0 + 4 * LU_COL ? 0 : 1);
}

/* The four encoders turn the controls of the active group. */
static void lira_knob(int knob, int counts)
{
    struct lu_spec sp[LU_MAXS];
    int sel = lira_ui_sel(-1), bank = (sel >> 4) & 7, n = lu_specs(sel, sp);
    for (int i = 0; i < n; i++)
        if (sp[i].bank == bank && sp[i].enc == knob) {
            int d = counts / 8, chord = lira_ui_chord_get(150);
            if (d == 0)
                d = counts > 0 ? 1 : -1;
            if (sp[i].kind == LK_NOTE || (sp[i].kind == LK_KNOB && sp[i].id < 8 && chord)) {
                int v = sp[i].kind == LK_NOTE ? sp[i].id - 200 : sp[i].id, st = lira_ui_accum(knob, counts, 100);
                if (chord && st)
                    lira_ui_note_set(v, lira_ui_note(v) + st);       /* chromatic, a step per ~100 counts */
            } else if (sp[i].kind == LK_BTN || sp[i].kind == LK_STEP) {   /* like the stock lists: a step per ~300 counts */
                int st = lira_ui_accum(knob, counts, 300);
                if (sp[i].kind == LK_STEP && st)
                    lira_ui_chord_set(sp[i].id, lira_ui_chord_get(sp[i].id) + st);
                for (int k = 0; sp[i].kind == LK_BTN && k < (st < 0 ? -st : st) && k < 3; k++)
                    lu_press(&sp[i], st > 0 ? 1 : -1);
            } else if (!(sp[i].id < 8 && chord)) {
                lira_ui_set(sp[i].id, lira_ui_get(sp[i].id) + d);
            }
            lira_ui_last(sp[i].id);
            return;
        }
}
