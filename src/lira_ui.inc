/* The LIRA page (mode M_LIRA of the Looper page), included by looper_page.c where its drawing helpers are in scope.
 * Opened by the PAD button (second press on the pad screen). The engine is lira_pad.c's page instance, which the looper's
 * output stage runs: no pad or sample needed. Everything is a knob (touch it and drag up or down) or a button.
 *   bottom (always)  the eight voices: a TUNE knob each, in its pair's colour, and a plate below it (tap: on / off; the top-right
 *                    switch makes the plates HOLD instead). The eight hardware buttons are the same eight plates.
 *   top (footer tabs)  FM: per voice pair FM source, TOUCH, MOD, SHARP;  DELAY: Time 1/2, Mod 1/2, Ext In, Vibrato, Feedback, Mix;
 *                    TONE: Hold 1-4 / 5-8, Pitch 1-4 / 5-8, Drive, Dist Mix, LFO A / B, Volume.
 * The four encoders turn the knobs with pink labels, which sit where the encoders are (two left, two right): on FM the tune of
 * voices 1-4 (BACK: 5-8), on DELAY Time 1, Time 2 (left) and Feedback, Mix (right), on TONE Hold 1-4 / 5-8 (left) and
 * Pitch 1-4 / 5-8 (right). The top bar shows the last touched knob and its value.
 */
#define LU_LEVEL 120
#define LU_MAXS  20
#define LU_STRIP 114                                              /* the voice knobs start here (d) */
#define LU_PLATE 169

struct lu_spec {
    uint16_t id;
    int16_t x, d, w, h;
    int8_t knob;                                                  /* post-swap knob 0 bottom-left, 1 top-left, 2 top-right, 3 bottom-right; -1 none */
    uint8_t col, r;
    const char *name;
};
static const char *const lu_tabname[3] = {"VOICES FM", "DELAY", "TONE"};
static const char *const lu_touch[3] = {"SLOW", "FAST", "FOLLOW"};
static const char *const lu_src[8] = {"FM SELF", "FM OFF", "FM LFO", "FM EXT", "FM P1-2", "FM P3-4", "FM P5-6", "FM P7-8"};
/* colours: the firmware's palette is fixed and mostly neon, so only its quiet entries are used: greys, a dark teal and an olive */
#define LU_CARD  0x19                                             /* 2b2b2b: the card behind a knob */
#define LU_LINE  0x10                                             /* 444444: hairlines, frames */
#define LU_DIM   0x16                                             /* aaaaaa: labels */
#define LU_ACC1  0x1a                                             /* 046b7a: dark teal */
#define LU_ACC2  0x1c                                             /* 777100: olive */
static const uint8_t lu_pair_col[4] = {LU_ACC1, LU_ACC2, LU_ACC1, LU_ACC2};

#define LU_SP(i, I, X, D, W, H, K, C, R, N) o[i] = (struct lu_spec){I, X, D, W, H, K, C, R, N}

static int lu_specs(int sel, struct lu_spec *o)
{
    int page = sel & 15, bank = (sel >> 4) & 1, n = 0;
    static const char *const vn[8] = {"1", "2", "3", "4", "5", "6", "7", "8"};
    for (int v = 0; v < 8; v++) {                                 /* the voice knobs, on every page */
        int kn = page == 0 && (v >> 2) == bank ? ((const int8_t[4]){1, 0, 2, 3})[v & 3] : -1;   /* v1 top-left, v2 bottom-left, v3 top-right, v4 bottom-right */
        LU_SP(n, v, 3 + v * 38, LU_STRIP, 38, 36, kn, lu_pair_col[v >> 1], 13, vn[v]);
        n++;
    }
    if (page == 0) {
        for (int p = 0; p < 4; p++) {
            LU_SP(n, 108 + p, 3 + p * 78, 52, 38, 46, -1, lu_pair_col[p], 12, "MOD");
            n++;
            LU_SP(n, 130 + p, 3 + p * 78 + 38, 52, 38, 46, -1, lu_pair_col[p], 12, "SHARP");
            n++;
        }
    } else if (page == 1) {
        static const uint16_t ids[7] = {16, 17, 125, 126, 21, 18, 19};
        static const char *const nm[7] = {"TIME 1", "TIME 2", "MOD 1", "MOD 2", "EXT IN", "FEEDBACK", "MIX"};
        static const int8_t kn[7] = {1, 0, -1, -1, -1, 2, 3};
        static const uint8_t cx[7] = {0, 0, 1, 1, 2, 3, 3}, cy[7] = {0, 1, 0, 1, 0, 0, 1};
        for (int i = 0; i < 7; i++) {                             /* four columns of two; the encoder knobs are in the outer columns, EXT IN stands between */
            LU_SP(n, ids[i], 3 + cx[i] * 77, i == 4 ? 41 : 17 + cy[i] * 48, 77, 46, kn[i], LU_ACC1, 14, nm[i]);
            n++;
        }
    } else {
        static const uint16_t ids[10] = {121, 122, 22, 23, 14, 15, LU_LEVEL, 13, 123, 124};
        static const char *const nm[10] = {"HOLD 1-4", "HOLD 5-8", "DRIVE", "DIST MIX", "LFO A", "LFO B", "VOLUME", "VIBRATO", "PITCH 1-4", "PITCH 5-8"};
        static const int8_t kn[10] = {1, 0, -1, -1, -1, -1, -1, -1, 2, 3};
        for (int i = 0; i < 10; i++) {                            /* five columns of two */
            LU_SP(n, ids[i], 3 + (i >> 1) * 62, 17 + (i & 1) * 48, 62, 46, kn[i], LU_ACC1, 12, nm[i]);
            n++;
        }
    }
    return n;
}

static void lu_plate_rect(int v, int *x, int *d, int *w, int *h)
{
    struct lay L;
    layout(0, &L);
    *x = 4 + v * 38;
    *d = LU_PLATE;
    *w = 36;
    *h = L.foot_y - 4 - LU_PLATE;
}

static void lu_pct(char **q, int v)
{
    *q = put_uint(*q, (unsigned)((v + 5) / 10));
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

static void lu_draw_knob(const struct lu_spec *s)
{
    int v = lira_ui_get(s->id), cx = s->x + s->w / 2, cy = s->d + s->r + 2;
    box(s->x + 1, s->d, s->w - 2, s->h, LU_CARD);
    lu_knob(cx, cy, s->r, v, s->col);
    text_c(s->x, cy + s->r + 3, s->w, s->name, s->knob >= 0 ? C_WHITE : LU_DIM, 1);
}

/* a rounded square, as three boxes */
static void lu_round(int x, int d, int w, int h, int col)
{
    box(x + 2, d, w - 4, h, col);
    box(x, d + 2, w, h - 4, col);
    box(x + 1, d + 1, w - 2, h - 2, col);
}

static void draw_lira(void)
{
    struct lay L;
    layout(0, &L);
    int sel = lira_ui_sel(-1), page = sel & 15, bank = (sel >> 4) & 1;
    struct lu_spec sp[LU_MAXS];
    int n = lu_specs(sel, sp);
    char b[24], *q;
    box(1, 1, P->w - 2, TOPBAR, C_BG);
    text(6, 4, "LIRA-8", C_WHITE, 1);
    int last = lira_ui_last(-1);
    q = b;
    for (int i = 0; i < n; i++)
        if (sp[i].id == last) {
            for (const char *t = sp[i].name; *t;)
                *q++ = *t++;
            *q++ = ' ';
            lu_pct(&q, lira_ui_get(last));
        }
    *q = 0;
    text(52, 4, *b ? b : (page == 0 ? (bank ? "ENCODERS: TUNE 5-8  (BACK: 1-4)" : "ENCODERS: TUNE 1-4  (BACK: 5-8)") : "ORGANISMIC SYNTHESIZER"),
         *b ? C_WHITE : C_GREY, 1);
    int mom = lira_ui_momentary(0);
    frame(P->w - 50, 2, 46, TOPBAR - 2, mom ? LU_ACC2 : LU_LINE, 1);
    text_c(P->w - 50, 4, 46, mom ? "HOLD" : "LATCH", LU_DIM, 1);
    box(1, TOPBAR + 2, P->w - 2, L.foot_y - TOPBAR - 3, C_BG);

    if (page == 0) {                                              /* the pair columns: a coloured rule, the two buttons */
        for (int p = 0; p < 4; p++) {
            int x = 3 + p * 78, t = lira_ui_get(100 + p), s = lira_ui_get(104 + p), col = lu_pair_col[p];
            box(x + 2, 16, 72, 2, col);
            box(x + 3, 21, 70, 12, LU_CARD);
            frame(x + 2, 20, 72, 14, s == 1 ? LU_LINE : col, 1);
            text_c(x + 2, 23, 72, lu_src[s], s == 1 ? C_GREY : C_WHITE, 1);
            box(x + 3, 37, 70, 12, t == 0 ? LU_CARD : col);
            frame(x + 2, 36, 72, 14, t == 0 ? LU_LINE : col, 1);
            text_c(x + 2, 39, 72, lu_touch[t], t == 0 ? LU_DIM : C_WHITE, 1);
        }
    }
    for (int i = 0; i < n; i++)
        if (sp[i].id >= 8 || i < 8)
            lu_draw_knob(&sp[i]);
    for (int g = 0; g < 4; g++)                                   /* the voices: a rule over each pair */
        box(3 + g * 76 + 2, LU_STRIP - 2, 72, 2, lu_pair_col[g]);
    for (int v = 0; v < 8; v++) {                                 /* the plates */
        int x, d, w, h, on = lira_ui_key_get(v), col = lu_pair_col[v >> 1];
        lu_plate_rect(v, &x, &d, &w, &h);
        lu_round(x, d, w, h, on ? col : LU_LINE);
        lu_round(x + 2, d + 2, w - 4, h - 4, on ? col : LU_CARD);
        q = put_uint(b, (unsigned)(v + 1));
        *q = 0;
        text_c(x, d + h / 2 - 8, w, b, on ? C_WHITE : C_GREY, 2);
    }
}

/* the footer: the page tabs (the stock FX button sits at the right, drawn by the caller) */
static void lira_footer(const struct lay *L)
{
    int page = lira_ui_sel(-1) & 15;
    for (int i = 0; i < 3; i++) {
        frame(3 + 82 * i, L->foot_y + 2, 80, FOOT - 3, page == i ? LU_ACC1 : LU_LINE, 1);
        text_c(3 + 82 * i, L->foot_y + 4, 80, lu_tabname[i], page == i ? C_WHITE : C_GREY, 1);
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

/* kind 0 down, 1 move, 2 up; x / d in page pixels. A knob turns by 80 px of travel up or down from where it was touched. */
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
                lira_ui_set(sp[i].id, lira_ui_drag(id, 4, 0, 0, 0, 0) + (lira_ui_drag(id, 3, 0, 0, 0, 0) - d) * 1000 / 80);
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
    if (page == 0 && d >= 20 && d < 50) {                         /* FM source / touch buttons of the four pairs */
        int p = (x - 3) / 78;
        if (x >= 3 && p < 4) {
            if (d < 35)
                lira_ui_set(104 + p, (lira_ui_get(104 + p) + 1) & 7);
            else
                lira_ui_set(100 + p, (lira_ui_get(100 + p) + 1) % 3);
        }
        return;
    }
    for (int i = 0; i < n; i++)
        if (x >= sp[i].x && x < sp[i].x + sp[i].w && d >= sp[i].d && d < sp[i].d + sp[i].h) {
            lira_ui_last(sp[i].id);
            lira_ui_drag(id, 0, 3, i, d, lira_ui_get(sp[i].id));
            return;
        }
}

/* The four encoders turn the knobs with pink labels. */
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
            lira_ui_last(sp[i].id);
            return;
        }
}
