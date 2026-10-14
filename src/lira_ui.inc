/* The LIRA page (mode M_LIRA of the Looper page), included by looper_page.c where its drawing helpers are in scope.
 * Opened by the PAD button (second press on the pad screen). The engine is lira_pad.c's page instance, which the looper's
 * output stage runs: no pad or sample needed.
 *   bottom (always)  the eight voices: a vertical TUNE fader each (hairline, ticks, round handle, in the pair's colour) and a big plate
 *                    below it (tap: on / off; the top-right switch makes the plates HOLD instead). The eight hardware buttons are
 *                    the same eight plates.
 *   top (footer tabs)  FM: per voice pair FM source, TOUCH, MOD, SHARP;  DELAY: Time 1/2, Mod 1/2, Feedback, Mix, Ext In, Vibrato;
 *                    TONE: Hold 1-4 / 5-8, Pitch 1-4 / 5-8, Drive, Dist Mix, Volume, LFO A / B.
 * The four encoders (the sliders with pink frames; two left, two right as the encoders sit): on FM the tune of voices 1-4 (BACK:
 * 5-8), on DELAY Time 1, Time 2, Feedback, Mix, on TONE Hold 1-4, Hold 5-8, Pitch 1-4, Pitch 5-8.
 */
#define LU_LEVEL 120
#define LU_MAXS  17
#define LU_STRIP 116                                              /* the voice faders start here (d) */
#define LU_PLATE 169
#define LU_ST_H  0                                                /* styles: a stacked horizontal slider, a vertical fader */
#define LU_ST_V  1

struct lu_spec {
    uint16_t id;
    int16_t x, d, w, h;
    int8_t knob;                                                  /* post-swap knob 0 bottom-left, 1 top-left, 2 top-right, 3 bottom-right; -1 none */
    uint8_t style, col;
    const char *name;
};
static const char *const lu_tabname[3] = {"VOICES FM", "DELAY", "TONE"};
static const char *const lu_touch[3] = {"SLOW", "FAST", "FOLLOW"};
static const char *const lu_src[8] = {"FM SELF", "FM OFF", "FM LFO", "FM EXT", "FM P1-2", "FM P3-4", "FM P5-6", "FM P7-8"};
static const uint8_t lu_pair_col[4] = {0x1b, 0x14, 0x17, 0x18};  /* the looper's four track colours: cyan, yellow, aqua, purple */

#define LU_SP(i, I, X, D, W, H, K, S, C, N) o[i] = (struct lu_spec){I, X, D, W, H, K, S, C, N}

static int lu_specs(int sel, struct lu_spec *o)
{
    int page = sel & 15, bank = (sel >> 4) & 1, cw = (P->w - 9) / 2, rx = 3 + cw + 3, n = 0;
    static const char *const vn[8] = {"1", "2", "3", "4", "5", "6", "7", "8"};
    for (int v = 0; v < 8; v++) {                                 /* the voice faders, on every page */
        int kn = page == 0 && (v >> 2) == bank ? ((const int8_t[4]){1, 0, 2, 3})[v & 3] : -1;   /* v1 top-left, v2 bottom-left, v3 top-right, v4 bottom-right */
        LU_SP(n, v, 3 + v * 38, LU_STRIP, 36, 50, kn, LU_ST_V, lu_pair_col[v >> 1], vn[v]);
        n++;
    }
    if (page == 0) {
        static const char *const mn[4] = {"MOD", "MOD", "MOD", "MOD"};
        for (int p = 0; p < 4; p++) {
            LU_SP(n, 108 + p, 3 + p * 78, 52, 76, 19, -1, LU_ST_H, lu_pair_col[p], mn[p]);
            n++;
            LU_SP(n, 130 + p, 3 + p * 78, 73, 76, 19, -1, LU_ST_H, lu_pair_col[p], "SHARP");
            n++;
        }
    } else if (page == 1) {
        static const uint16_t ids[8] = {16, 17, 125, 126, 18, 19, 21, 13};
        static const char *const nm[8] = {"TIME 1", "TIME 2", "MOD 1", "MOD 2", "FEEDBACK", "MIX", "EXT IN", "VIBRATO"};
        static const int8_t kn[8] = {1, 0, -1, -1, 2, 3, -1, -1};
        for (int i = 0; i < 8; i++) {
            LU_SP(n, ids[i], i < 4 ? 3 : rx, 17 + (i & 3) * 24, cw, 20, kn[i], LU_ST_H, 0x1b, nm[i]);
            n++;
        }
    } else {
        static const uint16_t ids[9] = {121, 122, 22, 23, 14, 123, 124, LU_LEVEL, 15};
        static const char *const nm[9] = {"HOLD 1-4", "HOLD 5-8", "DRIVE", "DIST MIX", "LFO A", "PITCH 1-4", "PITCH 5-8", "VOLUME", "LFO B"};
        static const int8_t kn[9] = {1, 0, -1, -1, -1, 2, 3, -1, -1};
        static const uint8_t row[9] = {0, 1, 2, 3, 4, 0, 1, 2, 3};
        for (int i = 0; i < 9; i++) {
            LU_SP(n, ids[i], i < 5 ? 3 : rx, 17 + row[i] * 19, cw, 18, kn[i], LU_ST_H, 0x1b, nm[i]);
            n++;
        }
    }
    return n;
}

static void lu_plate_rect(int v, int *x, int *d, int *w, int *h)
{
    struct lay L;
    layout(0, &L);
    *x = 3 + v * 38;
    *d = LU_PLATE;
    *w = 36;
    *h = L.foot_y - 4 - LU_PLATE;
}

/* the track of a slider: where 0 and 1000 are (x for horizontal ones, d for vertical ones) */
static void lu_track(const struct lu_spec *s, int *lo, int *hi)
{
    if (s->style == LU_ST_V) {
        *lo = s->d + s->h - 5;
        *hi = s->d + 12;
    } else {
        *lo = s->x + 4;
        *hi = s->x + s->w - 5;
    }
}

static void lu_pct(char **q, int v)
{
    *q = put_uint(*q, (unsigned)((v + 5) / 10));
}

static void lu_draw_slider(const struct lu_spec *s)
{
    int v = lira_ui_get(s->id), lo, hi, col = s->col, pos;
    char b[8], *q = b;
    lu_track(s, &lo, &hi);
    lu_pct(&q, v);
    *q = 0;
    box(s->x, s->d, s->w, s->h, C_BG);
    if (s->style == LU_ST_V) {
        int cx = s->x + s->w / 2;
        pos = lo + (hi - lo) * v / 1000;
        for (int t = 0; t <= 4; t++)
            box(cx - 3, lo + (hi - lo) * t / 4, 7, 1, C_RAIL);
        vline(cx, hi, lo - hi + 1, C_RAIL);
        box(cx, pos, 2, lo - pos + 1, col);                       /* the track colour up to the handle */
        dot(cx, pos, C_WHITE);
        text_c(s->x, s->d + 1, s->w, b, C_LIGHT, 1);
    } else {
        int cy = s->d + s->h - 4;
        pos = lo + (hi - lo) * v / 1000;
        for (int t = 0; t <= 4; t++)
            box(lo + (hi - lo) * t / 4, cy - 2, 1, 5, C_RAIL);
        hline(lo, cy, hi - lo + 1, C_RAIL);
        box(lo, cy, pos - lo + 1, 2, col);
        dot(pos, cy, C_WHITE);
        text(s->x + 2, s->d + 1, s->name, C_LIGHT, 1);
        text(s->x + s->w - 2 - text_w(b, 1), s->d + 1, b, C_WHITE, 1);
    }
    if (s->knob >= 0)
        frame(s->x, s->d, s->w, s->h, C_PINK, 1);
}

static void draw_lira(void)
{
    struct lay L;
    layout(0, &L);
    int sel = lira_ui_sel(-1), page = sel & 15, bank = (sel >> 4) & 1, cw = (P->w - 9) / 2, rx = 3 + cw + 3;
    struct lu_spec sp[LU_MAXS];
    int n = lu_specs(sel, sp);
    char b[24];
    box(1, 1, P->w - 2, TOPBAR, C_BG);
    text(6, 4, "LIRA-8", C_CYAN, 1);
    text(52, 4, page == 0 ? (bank ? "ENCODERS: TUNE 5-8 (BACK: 1-4)" : "ENCODERS: TUNE 1-4 (BACK: 5-8)") :
                            page == 1 ? "ENCODERS: TIME 1/2 FEEDBACK MIX" : "ENCODERS: HOLD PITCH", C_GREY, 1);
    int mom = lira_ui_momentary(0);
    frame(P->w - 50, 2, 46, TOPBAR - 2, C_RAIL, 1);
    text_c(P->w - 50, 4, 46, mom ? "HOLD" : "LATCH", C_YELLOW, 1);
    box(1, TOPBAR + 2, P->w - 2, L.foot_y - TOPBAR - 3, C_BG);

    for (int i = 0; i < n; i++)
        lu_draw_slider(&sp[i]);
    if (page == 0) {
        for (int p = 0; p < 4; p++) {
            int x = 3 + p * 78, t = lira_ui_get(100 + p), s = lira_ui_get(104 + p), col = lu_pair_col[p];
            frame(x, 17, 76, 15, s == 1 ? C_RAIL : col, 1);
            text_c(x, 21, 76, lu_src[s], s == 1 ? C_GREY : C_WHITE, 1);
            frame(x, 34, 76, 15, t == 0 ? C_RAIL : col, 1);
            text_c(x, 38, 76, lu_touch[t], t == 0 ? C_LIGHT : t == 1 ? C_YELLOW : C_GREEN, 1);
        }
    }
    (void)rx;
    for (int v = 0; v < 8; v++) {                                 /* the plates */
        int x, d, w, h, on = lira_ui_key_get(v), col = lu_pair_col[v >> 1];
        char *q = b;
        lu_plate_rect(v, &x, &d, &w, &h);
        box(x, d, w, h, on ? col : C_BG);
        frame(x, d, w, h, col, 1);
        q = put_uint(q, (unsigned)(v + 1));
        *q = 0;
        text_c(x, d + h / 2 - 8, w, b, on ? C_BLACK : col, 2);
    }
}

/* the footer: the page tabs (the stock FX button sits at the right, drawn by the caller) */
static void lira_footer(const struct lay *L)
{
    int page = lira_ui_sel(-1) & 15;
    for (int i = 0; i < 3; i++) {
        frame(3 + 82 * i, L->foot_y + 2, 80, FOOT - 3, page == i ? C_CYAN : C_RAIL, 1);
        text_c(3 + 82 * i, L->foot_y + 4, 80, lu_tabname[i], page == i ? C_CYAN : C_GREY, 1);
    }
}

static void lira_page_set(int p, int bank)
{
    lira_ui_sel((p & 15) | (bank & 1) << 4);
    P->entered = 0;
    P->sig = 0;
}

static int lira_footer_hit(int x)
{
    int i = (x - 3) / 82;
    if (x < 3 || i > 2 || (x - 3) % 82 >= 80)
        return 0;
    lira_page_set(i, (lira_ui_sel(-1) >> 4) & 1);
    return 1;
}

/* BACK: the other four voices for the encoders (from the other pages: back to FM) */
static void lira_back(void)
{
    int sel = lira_ui_sel(-1), page = sel & 15, bank = (sel >> 4) & 1;
    lira_page_set(0, page == 0 ? !bank : bank);
}

static int lu_value(const struct lu_spec *s, int x, int d)
{
    int lo, hi;
    lu_track(s, &lo, &hi);
    int p = s->style == LU_ST_V ? d : x, v = (p - lo) * 1000 / (hi - lo);
    return v < 0 ? 0 : v > 1000 ? 1000 : v;
}

/* kind 0 down, 1 move, 2 up; x / d in page pixels. */
static void lira_touch(int kind, int id, int x, int d)
{
    int sel = lira_ui_sel(-1), page = sel & 15;
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
            int i = lira_ui_drag(id, 2, 0, 0, 0, 0);
            if (i < n)
                lira_ui_set(sp[i].id, lu_value(&sp[i], x, d));
        }
        return;
    }
    lira_ui_drag(id, 0, 0, 0, 0, 0);
    if (d <= TOPBAR + 1) {
        if (x >= P->w - 50)
            lira_ui_momentary(1);
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
    if (page == 0 && d >= 17 && d < 49) {                         /* FM source / touch buttons of the four pairs */
        int p = (x - 3) / 78;
        if (x >= 3 && p < 4) {
            if (d < 33)
                lira_ui_set(104 + p, (lira_ui_get(104 + p) + 1) & 7);
            else
                lira_ui_set(100 + p, (lira_ui_get(100 + p) + 1) % 3);
        }
        return;
    }
    for (int i = 0; i < n; i++)
        if (x >= sp[i].x - 2 && x < sp[i].x + sp[i].w + 2 && d >= sp[i].d - 1 && d < sp[i].d + sp[i].h + 1) {
            lira_ui_set(sp[i].id, lu_value(&sp[i], x, d));
            lira_ui_drag(id, 0, 3, i, 0, 0);
            return;
        }
}

/* The four encoders turn the sliders with pink frames. */
static void lira_knob(int knob, int counts)
{
    struct lu_spec sp[LU_MAXS];
    int n = lu_specs(lira_ui_sel(-1), sp);
    for (int i = 0; i < n; i++)
        if (sp[i].knob == knob) {
            int d = counts / 8;
            if (d == 0)
                d = counts > 0 ? 1 : -1;
            lira_ui_set(sp[i].id, lira_ui_get(sp[i].id) + d);
            return;
        }
}
