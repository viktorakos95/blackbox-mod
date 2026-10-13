/* The LIRA page (mode M_LIRA of the Looper page), included by looper_page.c where its drawing helpers are in scope.
 * Opened by the PAD button (second press on the pad screen). The engine is lira_pad.c's page instance, which the looper's
 * output stage runs: no pad or sample needed.
 *   top bar    title, the selected dial's value, and the LATCH / HOLD switch (tap a plate: it stays on, or only while held)
 *   plates     the eight sensors: touch to energise a voice
 *   pairs      for each pair of voices (1-2, 3-4, ...): TOUCH (slow, fast, follow the input) and FM SOURCE (tap to step), and
 *              a slider for its FM depth
 *   dials      tuning, pitch, sharpness, hold, LFOs, delay, drive, input level: touch one to select it (pink) and drag up and
 *              down to turn it; the left-bottom knob turns the selected one, the others Time 1, Time 2 and Feedback.
 */
#define LU_PL_Y 17
#define LU_PL_H 40
#define LU_PR_Y 60
#define LU_PR_H 44
#define LU_DY0  106
#define LU_N    23
#define LU_LEVEL 120

struct lu_dial {
    uint8_t ctl;
    const char *name;
};
static const struct lu_dial lu_dials[LU_N] = {
    {0, "T1"}, {1, "T2"}, {2, "T3"}, {3, "T4"}, {4, "T5"}, {5, "T6"}, {6, "T7"}, {7, "T8"}, {8, "PITCH"},
    {LU_LEVEL, "LEVEL"}, {21, "EXT"}, {9, "SHARP"}, {10, "HOLD"}, {13, "VIB"}, {14, "LFO A"}, {15, "LFO B"}, {20, "DLYMOD"},
    {16, "TIME1"}, {17, "TIME2"}, {18, "FDBK"}, {19, "DLYMIX"}, {22, "DRIVE"}, {23, "DIST"}};
static const uint8_t lu_row0[3] = {0, 9, 17};
static const uint8_t lu_rown[3] = {9, 8, 6};
static const char *const lu_touch[3] = {"SLOW", "FAST", "FOLLOW"};
static const char *const lu_src[8] = {"SELF", "OFF", "LFO", "EXT", "PAIR1", "PAIR2", "PAIR3", "PAIR4"};

static int lu_dial_h(const struct lay *L)
{
    int pitch = (L->foot_y - 2 - LU_DY0) / 3;
    return pitch < 30 ? 30 : pitch;
}

static void draw_lira(void)
{
    struct lay L;
    layout(0, &L);
    char b[24], *q;
    box(1, 1, P->w - 2, TOPBAR, C_BG);
    text(6, 4, "LIRA-8", C_CYAN, 1);
    int sel = lira_ui_sel(-1);
    if (sel >= 0 && sel < LU_N) {
        q = b;
        for (const char *n = lu_dials[sel].name; *n;)
            *q++ = *n++;
        *q++ = ' ';
        q = put_uint(q, (unsigned)((lira_ui_get(lu_dials[sel].ctl) + 5) / 10));
        *q = 0;
        text(60, 4, b, C_PINK, 1);
    }
    int mom = lira_ui_momentary(0);
    frame(P->w - 62, 2, 58, TOPBAR - 2, C_RAIL, 1);
    text_c(P->w - 62, 4, 58, mom ? "HOLD" : "LATCH", C_YELLOW, 1);

    int pw = (P->w - 6) / 8;
    for (int k = 0; k < 8; k++) {
        int x = 3 + k * pw, on = lira_ui_key_get(k);
        box(x + 1, LU_PL_Y + 1, pw - 4, LU_PL_H - 2, on ? C_TEAL : C_BG);
        frame(x, LU_PL_Y, pw - 2, LU_PL_H, on ? C_CYAN : C_RAIL, 1);
        q = put_uint(b, (unsigned)(k + 1));
        *q = 0;
        text_c(x, LU_PL_Y + 12, pw - 2, b, on ? C_WHITE : C_LIGHT, 2);
    }
    int pcw = (P->w - 6) / 4;
    for (int g = 0; g < 4; g++) {
        int x = 3 + g * pcw, w = pcw - 2, h = w / 2;
        box(x + 1, LU_PR_Y + 1, w - 2, LU_PR_H - 2, C_BG);
        frame(x, LU_PR_Y, w, LU_PR_H, C_RAIL, 1);
        q = b;
        *q++ = (char)('0' + 2 * g + 1);
        *q++ = '-';
        *q++ = (char)('0' + 2 * g + 2);
        *q = 0;
        text(x + 4, LU_PR_Y + 3, b, C_LIGHT, 1);
        int t = lira_ui_get(100 + g), s = lira_ui_get(104 + g);
        frame(x + 1, LU_PR_Y + 12, h - 1, 14, t == 2 ? C_GREEN : t == 1 ? C_YELLOW : C_RAIL, 1);
        text_c(x + 1, LU_PR_Y + 15, h - 1, lu_touch[t], t == 0 ? C_LIGHT : C_WHITE, 1);
        frame(x + h, LU_PR_Y + 12, w - h - 1, 14, s == 1 ? C_RAIL : C_CYAN, 1);
        text_c(x + h, LU_PR_Y + 15, w - h - 1, lu_src[s], s == 1 ? C_GREY : C_WHITE, 1);
        int m = lira_ui_get(108 + g), bw = w - 6;
        frame(x + 3, LU_PR_Y + 30, bw, 11, C_RAIL, 1);
        box(x + 4, LU_PR_Y + 31, (bw - 2) * m / 1000, 9, C_PINK);
        text(x + 6, LU_PR_Y + 32, "FM", C_WHITE, 1);
    }
    int dh = lu_dial_h(&L);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < lu_rown[r]; c++) {
            int i = lu_row0[r] + c;
            dial_h(3 + c * DIAL, LU_DY0 + r * dh, dh - 1, lu_dials[i].name, (float)lira_ui_get(lu_dials[i].ctl) * .001f, i == sel, 0);
        }
}

static void lu_step(int ctl, int d)
{
    lira_ui_set(ctl, lira_ui_get(ctl) + d);
}

/* kind 0 down, 1 move, 2 up; x / d in page pixels. */
static void lira_touch(int kind, int id, int x, int d)
{
    struct lay L;
    layout(0, &L);
    int pw = (P->w - 6) / 8, pcw = (P->w - 6) / 4, dh = lu_dial_h(&L);
    if (kind == 2) {
        if (lira_ui_drag(id, 1, 0, 0, 0, 0) == 1)
            lira_ui_key(lira_ui_drag(id, 2, 0, 0, 0, 0), 0);
        lira_ui_drag(id, 0, 0, 0, 0, 0);
        return;
    }
    if (kind == 1) {
        int k = lira_ui_drag(id, 1, 0, 0, 0, 0), i = lira_ui_drag(id, 2, 0, 0, 0, 0);
        if (k == 2) {
            int w = pcw - 2 - 6, v = (x - (3 + i * pcw + 3)) * 1000 / (w > 0 ? w : 1);
            lira_ui_set(108 + i, v);
        } else if (k == 3) {
            lira_ui_set(lu_dials[i].ctl, lira_ui_drag(id, 4, 0, 0, 0, 0) + (lira_ui_drag(id, 3, 0, 0, 0, 0) - d) * 1000 / 80);
        }
        return;
    }
    lira_ui_drag(id, 0, 0, 0, 0, 0);
    if (d <= TOPBAR + 1) {
        if (x >= P->w - 62)
            lira_ui_momentary(1);
        return;
    }
    if (d >= LU_PL_Y && d < LU_PL_Y + LU_PL_H) {
        int k = (x - 3) / pw;
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
    if (d >= LU_PR_Y && d < LU_PR_Y + LU_PR_H) {
        int g = (x - 3) / pcw, lx = x - 3 - g * pcw, ld = d - LU_PR_Y;
        if (x < 3 || g > 3)
            return;
        if (ld >= 12 && ld < 28) {
            if (lx < (pcw - 2) / 2)
                lira_ui_set(100 + g, (lira_ui_get(100 + g) + 1) % 3);
            else
                lira_ui_set(104 + g, (lira_ui_get(104 + g) + 1) & 7);
        } else if (ld >= 28) {
            int w = pcw - 2 - 6;
            lira_ui_set(108 + g, (x - (3 + g * pcw + 3)) * 1000 / (w > 0 ? w : 1));
            lira_ui_drag(id, 0, 2, g, 0, 0);
        }
        return;
    }
    if (d >= LU_DY0 && x >= 3) {
        int r = (d - LU_DY0) / dh, c = (x - 3) / DIAL;
        if (r < 3 && c < lu_rown[r]) {
            int i = lu_row0[r] + c;
            lira_ui_sel(i);
            lira_ui_drag(id, 0, 3, i, d, lira_ui_get(lu_dials[i].ctl));
        }
    }
}

/* The four knobs: the first turns the selected dial, the others Time 1, Time 2 and Feedback. */
static void lira_knob(int knob, int counts)
{
    int i = knob == 0 ? lira_ui_sel(-1) : 16 + knob;           /* lu_dials[17..19] = TIME1, TIME2, FDBK */
    if (i < 0 || i >= LU_N)
        return;
    int d = counts / 8;
    if (d == 0)
        d = counts > 0 ? 1 : -1;
    lu_step(lu_dials[i].ctl, d);
}
