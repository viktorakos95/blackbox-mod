/* The LIRA page (mode M_LIRA of the Looper page), included by looper_page.c where its drawing helpers are in scope.
 * Opened by the PAD button (second press on the pad screen). The engine is lira_pad.c's page instance, which the looper's
 * output stage runs: no pad or sample needed. Four pages, picked by the footer tabs; BACK flips between VOICE 1-4 and VOICE 5-8:
 *   VOICE 1-4 / 5-8   two columns, one per voice pair: FM source and TOUCH buttons, MOD and SHARP sliders, the two voices' TUNE
 *                     sliders, and four big plates for the four voices
 *   DELAY             TIME 1, TIME 2, FEEDBACK, MIX big; MOD 1, MOD 2, EXT IN, VIBRATO small
 *   TONE              HOLD 1-4 / 5-8 and PITCH 1-4 / 5-8 big; DRIVE, DIST MIX, VOLUME, LFO A, LFO B small
 * The four big sliders of a page sit where the four encoders are (two left, two right: the top-left one is the top-left
 * encoder, and so on) and have the pink frames; everything is also touched and dragged. The eight hardware buttons toggle
 * the eight voices on every page.
 */
#define LU_LEVEL 120
#define LU_MAXS  9

struct lu_spec {
    uint16_t id;
    int16_t x, d, w, h;
    int8_t knob;                                                  /* post-swap knob 0 bottom-left, 1 top-left, 2 top-right, 3 bottom-right; -1 none */
    const char *name;
};
static const char *const lu_vname[8] = {"VOICE 1", "VOICE 2", "VOICE 3", "VOICE 4", "VOICE 5", "VOICE 6", "VOICE 7", "VOICE 8"};
static const char *const lu_tabname[4] = {"VOICE 1-4", "VOICE 5-8", "DELAY", "TONE"};
static const char *const lu_touch[3] = {"SLOW", "FAST", "FOLLOW"};
static const char *const lu_src[8] = {"FM SELF", "FM OFF", "FM LFO", "FM EXT", "FM P1-2", "FM P3-4", "FM P5-6", "FM P7-8"};

#define LU_SP(i, I, X, D, W, H, K, N) o[i] = (struct lu_spec){I, X, D, W, H, K, N}

static int lu_specs(int page, struct lu_spec *o)
{
    int cw = (P->w - 9) / 2, rx = 3 + cw + 3;
    if (page < 2) {
        for (int g = 0; g < 2; g++) {
            int x0 = g ? rx : 3, p = page * 2 + g, v = page * 4 + g * 2;
            LU_SP(g * 4 + 0, 108 + p, x0, 37, cw, 14, -1, "MOD");
            LU_SP(g * 4 + 1, 130 + p, x0, 53, cw, 14, -1, "SHARP");
            LU_SP(g * 4 + 2, v, x0, 70, cw, 36, g ? 2 : 1, lu_vname[v]);
            LU_SP(g * 4 + 3, v + 1, x0, 109, cw, 36, g ? 3 : 0, lu_vname[v + 1]);
        }
        return 8;
    }
    LU_SP(0, page == 2 ? 16 : 121, 3, 18, cw, 46, 1, page == 2 ? "TIME 1" : "HOLD 1-4");
    LU_SP(1, page == 2 ? 17 : 122, 3, 67, cw, 46, 0, page == 2 ? "TIME 2" : "HOLD 5-8");
    LU_SP(2, page == 2 ? 18 : 123, rx, 18, cw, 46, 2, page == 2 ? "FEEDBACK" : "PITCH 1-4");
    LU_SP(3, page == 2 ? 19 : 124, rx, 67, cw, 46, 3, page == 2 ? "MIX" : "PITCH 5-8");
    if (page == 2) {
        LU_SP(4, 125, 3, 118, cw, 18, -1, "MOD 1");
        LU_SP(5, 126, 3, 139, cw, 18, -1, "MOD 2");
        LU_SP(6, 21, rx, 118, cw, 18, -1, "EXT IN");
        LU_SP(7, 13, rx, 139, cw, 18, -1, "VIBRATO");
        return 8;
    }
    LU_SP(4, 22, 3, 118, cw, 18, -1, "DRIVE");
    LU_SP(5, 23, 3, 139, cw, 18, -1, "DIST MIX");
    LU_SP(6, 14, 3, 160, cw, 18, -1, "LFO A");
    LU_SP(7, LU_LEVEL, rx, 118, cw, 18, -1, "VOLUME");
    LU_SP(8, 15, rx, 139, cw, 18, -1, "LFO B");
    return 9;
}

static void lu_plate_rect(int page, int i, int *x, int *d, int *w, int *h)      /* page 0 / 1: four big plates i = 0..3; else eight small */
{
    struct lay L;
    layout(0, &L);
    if (page < 2) {
        *x = 3 + i * 78;
        *d = 150;
        *w = 76;
        *h = L.foot_y - 4 - 150;
    } else {
        *x = 3 + i * 38;
        *d = L.foot_y - 20;
        *w = 36;
        *h = 18;
    }
}

static void lu_pct(char **q, int v)
{
    *q = put_uint(*q, (unsigned)((v + 5) / 10));
}

static void lu_plate_draw(int page, int i)
{
    int x, d, w, h, k = page < 2 ? page * 4 + i : i, on = lira_ui_key_get(k);
    char b[4], *q;
    lu_plate_rect(page, i, &x, &d, &w, &h);
    box(x + 1, d + 1, w - 2, h - 2, on ? C_TEAL : C_BG);
    frame(x, d, w, h, on ? C_CYAN : C_RAIL, 1);
    q = put_uint(b, (unsigned)(k + 1));
    *q = 0;
    if (page < 2)
        text_c(x, d + h / 2 - 8, w, b, on ? C_WHITE : C_LIGHT, 2);
    else
        text_c(x, d + 5, w, b, on ? C_WHITE : C_LIGHT, 1);
}

static void draw_lira(void)
{
    struct lay L;
    layout(0, &L);
    int page = lira_ui_sel(-1) & 3, cw = (P->w - 9) / 2, rx = 3 + cw + 3;
    struct lu_spec sp[LU_MAXS];
    int n = lu_specs(page, sp);
    char b[24], *q;
    box(1, 1, P->w - 2, TOPBAR, C_BG);
    text(6, 4, "LIRA-8", C_CYAN, 1);
    text(52, 4, page < 2 ? "BACK = VOICES 1-4 / 5-8" : "BACK = VOICES", C_GREY, 1);
    int mom = lira_ui_momentary(0);
    frame(P->w - 50, 2, 46, TOPBAR - 2, C_RAIL, 1);
    text_c(P->w - 50, 4, 46, mom ? "HOLD" : "LATCH", C_YELLOW, 1);
    box(1, TOPBAR + 2, P->w - 2, L.foot_y - TOPBAR - 3, C_BG);

    for (int i = 0; i < n; i++) {
        const struct lu_spec *s = &sp[i];
        int v = lira_ui_get(s->id), fw = (s->w - 2) * v / 1000, big = s->knob >= 0;
        box(s->x + 1, s->d + 1, fw, s->h - 2, C_TEAL);
        box(s->x + 1 + fw, s->d + 1, 2, s->h - 2, C_WHITE);
        frame(s->x, s->d, s->w, s->h, big ? C_PINK : C_RAIL, 1);
        text(s->x + 4, s->d + (big ? 4 : (s->h - 8) / 2), s->name, C_WHITE, 1);
        q = b;
        lu_pct(&q, v);
        *q = 0;
        if (big)
            text(s->x + 4, s->d + s->h - 19, b, C_WHITE, 2);
        else
            text(s->x + s->w - 4 - text_w(b, 1), s->d + (s->h - 8) / 2, b, C_LIGHT, 1);
    }
    if (page < 2) {
        for (int g = 0; g < 2; g++) {
            int x0 = g ? rx : 3, p = page * 2 + g, t = lira_ui_get(100 + p), s = lira_ui_get(104 + p), hw = (cw - 4) / 2;
            frame(x0, 18, hw, 16, s == 1 ? C_RAIL : C_CYAN, 1);
            text_c(x0, 22, hw, lu_src[s], s == 1 ? C_GREY : C_WHITE, 1);
            frame(x0 + hw + 4, 18, hw, 16, t == 2 ? C_GREEN : t == 1 ? C_YELLOW : C_RAIL, 1);
            text_c(x0 + hw + 4, 22, hw, lu_touch[t], t == 0 ? C_LIGHT : C_WHITE, 1);
        }
        for (int i = 0; i < 4; i++)
            lu_plate_draw(page, i);
    } else {
        for (int i = 0; i < 8; i++)
            lu_plate_draw(page, i);
    }
}

/* the footer: the four page tabs (the stock FX button sits at the right, drawn by the caller) */
static void lira_footer(const struct lay *L)
{
    int page = lira_ui_sel(-1) & 3;
    for (int i = 0; i < 4; i++) {
        frame(3 + 62 * i, L->foot_y + 2, 60, FOOT - 3, page == i ? C_CYAN : C_RAIL, 1);
        text_c(3 + 62 * i, L->foot_y + 4, 60, lu_tabname[i], page == i ? C_CYAN : C_GREY, 1);
    }
}

static void lira_page_set(int p)
{
    lira_ui_sel(p & 3);
    P->entered = 0;
    P->sig = 0;
}

static int lira_footer_hit(int x)
{
    int i = (x - 3) / 62;
    if (x < 3 || i > 3 || (x - 3) % 62 >= 60)
        return 0;
    lira_page_set(i);
    return 1;
}

/* BACK: between the two voice pages (from the other pages back to VOICE 1-4) */
static void lira_back(void)
{
    int p = lira_ui_sel(-1) & 3;
    lira_page_set(p == 0 ? 1 : 0);
}

static int lu_value(const struct lu_spec *s, int x)
{
    int v = (x - s->x - 1) * 1000 / (s->w > 2 ? s->w - 2 : 1);
    return v < 0 ? 0 : v > 1000 ? 1000 : v;
}

/* kind 0 down, 1 move, 2 up; x / d in page pixels. */
static void lira_touch(int kind, int id, int x, int d)
{
    int page = lira_ui_sel(-1) & 3, cw = (P->w - 9) / 2, rx = 3 + cw + 3;
    struct lu_spec sp[LU_MAXS];
    int n = lu_specs(page, sp);
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
                lira_ui_set(sp[i].id, lu_value(&sp[i], x));
        }
        return;
    }
    lira_ui_drag(id, 0, 0, 0, 0, 0);
    if (d <= TOPBAR + 1) {
        if (x >= P->w - 50)
            lira_ui_momentary(1);
        return;
    }
    for (int i = 0; i < (page < 2 ? 4 : 8); i++) {                /* plates */
        int px, pd, pw, ph, k = page < 2 ? page * 4 + i : i;
        lu_plate_rect(page, i, &px, &pd, &pw, &ph);
        if (x >= px && x < px + pw && d >= pd && d < pd + ph) {
            if (lira_ui_momentary(0)) {
                lira_ui_key(k, 1);
                lira_ui_drag(id, 0, 1, k, 0, 0);
            } else {
                lira_ui_key(k, !lira_ui_key_get(k));
            }
            return;
        }
    }
    if (page < 2 && d >= 18 && d < 34) {                          /* FM source / touch buttons */
        int g = x >= rx ? 1 : 0, p = page * 2 + g, x0 = g ? rx : 3, hw = (cw - 4) / 2;
        if (x >= x0 && x < x0 + cw) {
            if (x < x0 + hw + 2)
                lira_ui_set(104 + p, (lira_ui_get(104 + p) + 1) & 7);
            else
                lira_ui_set(100 + p, (lira_ui_get(100 + p) + 1) % 3);
        }
        return;
    }
    for (int i = 0; i < n; i++)
        if (x >= sp[i].x - 2 && x < sp[i].x + sp[i].w + 2 && d >= sp[i].d - 1 && d < sp[i].d + sp[i].h + 1) {
            lira_ui_set(sp[i].id, lu_value(&sp[i], x));
            lira_ui_drag(id, 0, 3, i, 0, 0);
            return;
        }
}

/* The four encoders turn the page's four big sliders (two left, two right, as they sit). */
static void lira_knob(int knob, int counts)
{
    struct lu_spec sp[LU_MAXS];
    int n = lu_specs(lira_ui_sel(-1) & 3, sp);
    for (int i = 0; i < n; i++)
        if (sp[i].knob == knob) {
            int d = counts / 8;
            if (d == 0)
                d = counts > 0 ? 1 : -1;
            lira_ui_set(sp[i].id, lira_ui_get(sp[i].id) + d);
            return;
        }
}
