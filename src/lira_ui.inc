/* The LIRA page (mode M_LIRA of the Looper page), included by looper_page.c where its drawing helpers are in scope.
 * Opened by the PAD button (second press on the pad screen). The engine is lira_pad.c's page instance, which the looper's
 * output stage runs: no pad or sample needed. The layout follows the plugin's:
 *   top        HYPER-LFO (two faders), MOD-DELAY (Mod 1/2, Ext, Vibrato / Time 1/2, Feedback, Mix), DISTORTION (Drive, Mix, Vol)
 *   middle     HOLD 1-4 / 5-8 and PITCH 1-4 / 5-8
 *   four columns, one per voice pair: FM source (tap to step), MOD and SHARP sliders, the two TUNE faders, TOUCH (tap to step)
 *   bottom     the eight plates (tap: voice on / off; the top-right switch makes them HOLD instead)
 * Every slider is touched and dragged. The four encoders turn four sliders of the active group at a time (the pink frames; a
 * touch on a slider makes its group the active one); INFO is Shift: the next four of the group (voices 5-8 for TUNE, SHARP for
 * the pairs, Feedback / Mix / Ext / Vibrato for the delay group).
 */
#define LU_NS 33
#define LU_LEVEL 120

struct lu_geo {
    int a_y, b_y, c_y, c_h, pl_y, dx;
};

struct lu_slider {
    uint16_t id;
    uint8_t vert;
    const char *name;
};
static const struct lu_slider lu_sl[LU_NS] = {
    {0, 1, "1"}, {1, 1, "2"}, {2, 1, "3"}, {3, 1, "4"}, {4, 1, "5"}, {5, 1, "6"}, {6, 1, "7"}, {7, 1, "8"},
    {108, 0, "MOD"}, {109, 0, "MOD"}, {110, 0, "MOD"}, {111, 0, "MOD"},
    {130, 0, "SHARP"}, {131, 0, "SHARP"}, {132, 0, "SHARP"}, {133, 0, "SHARP"},
    {121, 0, "HOLD 1-4"}, {122, 0, "HOLD 5-8"}, {123, 0, "PITCH 1-4"}, {124, 0, "PITCH 5-8"},
    {125, 0, "MOD-1"}, {126, 0, "MOD-2"}, {16, 0, "TIME-1"}, {17, 0, "TIME-2"},
    {18, 0, "FEEDBACK"}, {19, 0, "MIX"}, {21, 0, "EXT IN"}, {13, 0, "VIBRATO"},
    {14, 1, "F-A"}, {15, 1, "F-B"}, {22, 1, "DRV"}, {23, 1, "MIX"}, {LU_LEVEL, 1, "VOL"}};
/* the four encoders: slot 0-3, with Shift slot 4-7, of each group */
static const int8_t lu_grp[5][8] = {{0, 1, 2, 3, 4, 5, 6, 7},
                                    {8, 9, 10, 11, 12, 13, 14, 15},
                                    {16, 17, 18, 19, -1, -1, -1, -1},
                                    {20, 21, 22, 23, 24, 25, 26, 27},
                                    {28, 29, 30, 31, 32, -1, -1, -1}};
static const char *const lu_gname[5][2] = {{"TUNE 1-4", "TUNE 5-8"}, {"MOD 1-4", "SHARP 1-4"}, {"HOLD PITCH", "-"},
                                           {"DLY MOD TIME", "FB MIX EXT VIB"}, {"LFO DRV DIST", "VOL"}};
static const char *const lu_touch[3] = {"SLOW", "FAST", "FOLLOW"};
static const char *const lu_src[8] = {"FM SELF", "FM OFF", "FM LFO", "FM EXT", "FM P1-2", "FM P3-4", "FM P5-6", "FM P7-8"};

static void lu_geo_get(struct lu_geo *G)
{
    struct lay L;
    layout(0, &L);
    G->a_y = 17;
    G->b_y = 65;
    G->c_y = 95;
    G->pl_y = L.foot_y - 26;
    G->c_h = G->pl_y - 2 - G->c_y;
    G->dx = P->w - 3 - 78;
}

static void lu_rect(const struct lu_geo *G, int i, int *x, int *d, int *w, int *h)
{
    int cw = (P->w - 6) / 4;
    if (i < 8) {                                                  /* tune faders, two per pair column */
        int g = i >> 1, k = i & 1;
        *x = 3 + g * cw + 5 + k * 36;
        *d = G->c_y + 38;
        *w = 31;
        *h = G->c_h - 38 - 14;
    } else if (i < 16) {                                          /* MOD, SHARP: a pair's column */
        int g = (i - 8) & 3;
        *x = 3 + g * cw + 2;
        *d = G->c_y + 13 + (i >= 12 ? 12 : 0);
        *w = cw - 5;
        *h = 11;
    } else if (i < 20) {                                          /* HOLD, PITCH: two columns */
        *x = 3 + ((i - 16) & 1) * 156;
        *d = G->b_y + (i >= 18 ? 14 : 0);
        *w = 152;
        *h = 12;
    } else if (i < 28) {                                          /* the mod-delay block: two columns of four rows */
        int colw = (G->dx - 3 - 46 - 3) / 2;
        int right = i >= 22 && i <= 25;                           /* right column: TIME-1 TIME-2 FEEDBACK MIX; left: MOD-1 MOD-2 EXT VIBRATO */
        int row = i == 20 ? 0 : i == 21 ? 1 : i == 26 ? 2 : i == 27 ? 3 : i - 22;
        *x = 46 + (right ? colw + 3 : 0);
        *d = G->a_y + row * 11;
        *w = colw;
        *h = 10;
    } else if (i < 30) {                                          /* HYPER-LFO */
        *x = 3 + (i - 28) * 20;
        *d = G->a_y;
        *w = 18;
        *h = 46;
    } else {                                                      /* DISTORTION */
        *x = G->dx + (i - 30) * 26;
        *d = G->a_y;
        *w = 24;
        *h = 46;
    }
}

static int lu_group_of(int i)
{
    for (int g = 0; g < 5; g++)
        for (int s = 0; s < 8; s++)
            if (lu_grp[g][s] == i)
                return g;
    return 0;
}

static int lu_hot(int i)
{
    int g = lira_ui_sel(-1), base = P->info_on ? 4 : 0;
    for (int s = 0; s < 4; s++)
        if (lu_grp[g][base + s] == i)
            return 1;
    return 0;
}

static void lu_pct(char **q, int v)
{
    *q = put_uint(*q, (unsigned)((v + 5) / 10));
}

static void draw_lira(void)
{
    struct lu_geo G;
    lu_geo_get(&G);
    char b[40], *q;
    box(1, 1, P->w - 2, TOPBAR, C_BG);
    text(6, 4, "LIRA-8", C_CYAN, 1);
    int grp = lira_ui_sel(-1), sh = P->info_on ? 1 : 0;
    q = b;
    for (const char *n = lu_gname[grp][sh]; *n;)
        *q++ = *n++;
    for (int s = 0; s < 4; s++) {
        int i = lu_grp[grp][sh * 4 + s];
        if (i >= 0) {
            *q++ = ' ';
            lu_pct(&q, lira_ui_get(lu_sl[i].id));
        }
    }
    *q = 0;
    text(46, 4, b, sh ? C_YELLOW : C_PINK, 1);
    int mom = lira_ui_momentary(0);
    frame(P->w - 50, 2, 46, TOPBAR - 2, C_RAIL, 1);
    text_c(P->w - 50, 4, 46, mom ? "HOLD" : "LATCH", C_YELLOW, 1);

    for (int i = 0; i < LU_NS; i++) {
        int x, d, w, h;
        lu_rect(&G, i, &x, &d, &w, &h);
        int v = lira_ui_get(lu_sl[i].id);
        box(x + 1, d + 1, w - 2, h - 2, C_BG);
        if (lu_sl[i].vert) {
            int fh = (h - 2) * v / 1000;
            box(x + 1, d + h - 1 - fh, w - 2, fh, C_TEAL);
            box(x + 1, d + h - 2 - fh, w - 2, 2, C_WHITE);
            text_c(x, d + h - 10, w, lu_sl[i].name, C_WHITE, 1);
        } else {
            int fw = (w - 2) * v / 1000;
            box(x + 1, d + 1, fw, h - 2, C_TEAL);
            box(x + fw, d + 1, 2, h - 2, C_WHITE);
            text(x + 3, d + (h - 8) / 2, lu_sl[i].name, C_WHITE, 1);
        }
        frame(x, d, w, h, lu_hot(i) ? C_PINK : C_RAIL, 1);
    }
    int cw = (P->w - 6) / 4;
    for (int g = 0; g < 4; g++) {
        int x = 3 + g * cw + 2, w = cw - 5, t = lira_ui_get(100 + g), s = lira_ui_get(104 + g);
        frame(x, G.c_y, w, 12, s == 1 ? C_RAIL : C_CYAN, 1);
        text_c(x, G.c_y + 2, w, lu_src[s], s == 1 ? C_GREY : C_WHITE, 1);
        frame(x, G.c_y + G.c_h - 12, w, 12, t == 2 ? C_GREEN : t == 1 ? C_YELLOW : C_RAIL, 1);
        text_c(x, G.c_y + G.c_h - 10, w, lu_touch[t], t == 0 ? C_LIGHT : C_WHITE, 1);
    }
    int pw = (P->w - 6) / 8;
    for (int k = 0; k < 8; k++) {
        int x = 3 + k * pw, on = lira_ui_key_get(k);
        box(x + 1, G.pl_y + 1, pw - 4, 22, on ? C_TEAL : C_BG);
        frame(x, G.pl_y, pw - 2, 24, on ? C_CYAN : C_RAIL, 1);
        q = put_uint(b, (unsigned)(k + 1));
        *q = 0;
        text_c(x, G.pl_y + 4, pw - 2, b, on ? C_WHITE : C_LIGHT, 2);
    }
}

/* the slider's value from a touch at x / d */
static int lu_value(const struct lu_geo *G, int i, int x, int d)
{
    int sx, sd, sw, sh;
    lu_rect(G, i, &sx, &sd, &sw, &sh);
    int v = lu_sl[i].vert ? (sd + sh - 1 - d) * 1000 / (sh > 2 ? sh - 2 : 1) : (x - sx - 1) * 1000 / (sw > 2 ? sw - 2 : 1);
    return v < 0 ? 0 : v > 1000 ? 1000 : v;
}

/* kind 0 down, 1 move, 2 up; x / d in page pixels. */
static void lira_touch(int kind, int id, int x, int d)
{
    struct lu_geo G;
    lu_geo_get(&G);
    if (kind == 2) {
        if (lira_ui_drag(id, 1, 0, 0, 0, 0) == 1)
            lira_ui_key(lira_ui_drag(id, 2, 0, 0, 0, 0), 0);
        lira_ui_drag(id, 0, 0, 0, 0, 0);
        return;
    }
    if (kind == 1) {
        if (lira_ui_drag(id, 1, 0, 0, 0, 0) == 3) {
            int i = lira_ui_drag(id, 2, 0, 0, 0, 0);
            lira_ui_set(lu_sl[i].id, lu_value(&G, i, x, d));
        }
        return;
    }
    lira_ui_drag(id, 0, 0, 0, 0, 0);
    if (d <= TOPBAR + 1) {
        if (x >= P->w - 50)
            lira_ui_momentary(1);
        return;
    }
    if (d >= G.pl_y) {                                            /* the plates */
        int pw = (P->w - 6) / 8, k = (x - 3) / pw;
        if (x < 3 || k > 7)
            return;
        if (lira_ui_momentary(0)) {
            lira_ui_key(k, 1);
            lira_ui_drag(id, 0, 1, k, 0, 0);
        } else {
            lira_ui_key(k, !lira_ui_key_get(k));
        }
        return;
    }
    if (d >= G.c_y && d < G.c_y + 13) {                           /* FM source buttons */
        int cw = (P->w - 6) / 4, g = (x - 3) / cw;
        if (x >= 3 && g < 4)
            lira_ui_set(104 + g, (lira_ui_get(104 + g) + 1) & 7);
        return;
    }
    if (d >= G.c_y + G.c_h - 13 && d < G.pl_y) {                  /* touch buttons */
        int cw = (P->w - 6) / 4, g = (x - 3) / cw;
        if (x >= 3 && g < 4)
            lira_ui_set(100 + g, (lira_ui_get(100 + g) + 1) % 3);
        return;
    }
    for (int i = 0; i < LU_NS; i++) {
        int sx, sd, sw, sh;
        lu_rect(&G, i, &sx, &sd, &sw, &sh);
        if (x >= sx - 2 && x < sx + sw + 2 && d >= sd - 1 && d < sd + sh + 1) {
            lira_ui_sel(lu_group_of(i));
            lira_ui_set(lu_sl[i].id, lu_value(&G, i, x, d));
            lira_ui_drag(id, 0, 3, i, 0, 0);
            return;
        }
    }
}

/* The four encoders: the active group's slots 0-3, or 4-7 with Shift (INFO). */
static void lira_knob(int knob, int counts)
{
    int i = lu_grp[lira_ui_sel(-1)][knob + (P->info_on ? 4 : 0)];
    if (i < 0)
        return;
    int d = counts / 8;
    if (d == 0)
        d = counts > 0 ? 1 : -1;
    lira_ui_set(lu_sl[i].id, lira_ui_get(lu_sl[i].id) + d);
}
