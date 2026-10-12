/*
 * Trigger2MIDI page: the third page of the SONG button (SAMPLR, TRIG, then the stock song screen). src/looper_page.c
 * draws the frame and the footer and hands over the area between the top bar and the footer, the touches there and the
 * four knobs. The footer's sheets: HITS, DETECT, OUT, CLK.
 *
 *   top bar    TRIG  L <hits> v<last>   R <hits> v<last>
 *   row        [IN L] [IN R]   [ON]   [TRS] [USB]        the input shown, its switch, its MIDI ports (every sheet)
 *   HITS       two pads that light up on a hit (colour = velocity, note and velocity in them), the input levels with the
 *              threshold, and the last 4 s of hits per input as bars (height = velocity). The knobs turn SENS, THRESH,
 *              RETRIG, MASK of the input shown, to set the threshold while watching the meter.
 *   DETECT     SENS THRESH RETRIG MASK / SCAN STRICT SPEED CURVE / BLEED RANGE FLOOR, and the meter
 *   OUT        NOTE VEL LEN CHAN / CC CC VAL / PAD PAD NOTE LEARN
 * On DETECT and OUT a tap on a row hands it to the four knobs (pink frame): knob n turns the row's n-th value (the
 * Looper page's knob order). NOTE OFF = no MIDI note (hits still show and still reach a pad); CC OFF = no CC; VEL / CC VAL /
 * LEN at their lowest = DYN. PAD: a pad of the Blackbox as a target: tap LEARN, hit a pad; PAD turns it off and on; PAD NOTE
 * is the note it gets. Knobs: 40 counts a step (the SAMPLR envelope's rate).
 *   CLK        a measurement, not a setting (src/clkmon.c): how the MIDI clock arriving on the TRS jack and on USB is seen,
 *              timings in microseconds; a tap anywhere on the sheet resets the numbers. No switch row, the knobs do nothing.
 */
#include <stdint.h>

#include "t2m_bb.h"

void lp_box(int x, int d, int w, int h, int color);
void lp_frame(int x, int d, int w, int h, int color, int th);
void lp_text(int x, int d, const char *s, int color, int scale);
void lp_text_c(int x, int d, int w, const char *s, int color, int scale);
void lp_hline(int x, int d, int w, int color);
void lp_vline(int x, int d, int h, int color);

/* palette (looper_page.c's) */
#define C_BG     0x02
#define C_BLACK  0x0e
#define C_RAIL   0x10
#define C_GREY   0x09
#define C_LIGHT  0x16
#define C_WHITE  0x0f
#define C_GREEN  0x0b
#define C_RED    0x0c
#define C_REC    0x06
#define C_YELLOW 0x14
#define C_CYAN   0x1b
#define C_PINK   0x20

#define TOP   16                 /* first line below the top bar */
#define BAR_H 20                 /* the switch row */
#define MET_H 12                 /* a meter */
#define STEP_COUNTS 40
#define LIT_BLOCKS  30           /* a pad is lit this long after a hit (160 ms) */

enum { SH_HITS, SH_DETECT, SH_OUT, SH_CLK, SHEETS };
enum { V_SENS, V_THRESH, V_RETRIG, V_MASK, V_SCAN, V_STRICT, V_SPEED, V_CURVE, V_NOTE, V_VEL, V_LEN, V_CHAN, V_CC, V_CCVAL,
       V_BLEED, V_RANGE, V_PAD, V_PNOTE, V_LEARN, V_FLOOR, VALUES, V_NONE = -1 };
static const char *const val_name[VALUES] = {"SENS", "THRESH", "RETRIG", "MASK", "SCAN", "STRICT", "SPEED", "CURVE",
                                             "NOTE", "VEL", "LEN", "CHAN", "CC", "CC VAL", "BLEED", "RANGE",
                                             "PAD", "PAD NOTE", "", "FLOOR"};
/* the settings sheets: three rows of four; the HITS sheet's knobs are DETECT's first row */
static const int8_t sheet_vals[2][12] = {
    {V_SENS, V_THRESH, V_RETRIG, V_MASK, V_SCAN, V_STRICT, V_SPEED, V_CURVE, V_BLEED, V_RANGE, V_FLOOR, V_NONE},
    {V_NOTE, V_VEL, V_LEN, V_CHAN, V_CC, V_CCVAL, V_NONE, V_NONE, V_PAD, V_PNOTE, V_LEARN, V_NONE},
};

#define UI (&m->ui)

/* ---- text */
static char *put_s(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

static char *put_u(char *p, unsigned v)
{
    char t[10];
    int n = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = t[--n];
    return p;
}

static char *put_f(char *p, float v, int dec)        /* v >= 0, dec 0..2 */
{
    unsigned sc = dec == 2 ? 100 : dec == 1 ? 10 : 1;
    unsigned n = (unsigned)(v * (float)sc + .5f);
    p = put_u(p, n / sc);
    if (dec) {
        *p++ = '.';
        unsigned f = n % sc;
        if (dec == 2 && f < 10)
            *p++ = '0';
        p = put_u(p, f);
    }
    return p;
}

static char *put_note(char *p, int n)                 /* Max / Yamaha naming: 60 = C3, 38 = D1 */
{
    static const char *const nm[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    p = put_s(p, nm[n % 12]);
    int o = n / 12 - 2;
    if (o < 0) {
        *p++ = '-';
        o = -o;
    }
    return put_u(p, (unsigned)o);
}

static void value_text(const struct t2m_mem *m, int k, int v, char *b)
{
    const t2m_params *q = &m->p[k];
    char *p = b;
    switch (v) {
    case V_SENS: p = put_f(p, q->sens, 2); break;
    case V_THRESH: p = put_f(p, q->thresh, 2); break;
    case V_RETRIG: p = put_f(p, q->retrig, 2); break;
    case V_MASK: p = put_u(p, (unsigned)(q->mask_ms + .5f)); p = put_s(p, "ms"); break;
    case V_SCAN: p = put_f(p, q->scan_ms, 1); p = put_s(p, "ms"); break;
    case V_STRICT: p = put_f(p, q->strict_db, 1); p = put_s(p, "dB"); break;
    case V_SPEED: p = put_f(p, q->speed_ms, 1); p = put_s(p, "ms"); break;
    case V_CURVE: p = put_f(p, q->curve, 2); break;
    case V_NOTE:
        if (!q->note_on)
            p = put_s(p, "OFF");
        else {
            p = put_note(p, q->note);
            *p++ = ' ';
            p = put_u(p, q->note);
        }
        break;
    case V_VEL: p = q->vel_mode ? put_u(p, q->vel_fixed) : put_s(p, "DYN"); break;
    case V_LEN: if (q->len_mode) { p = put_u(p, q->len_ms); p = put_s(p, "ms"); } else p = put_s(p, "DYN"); break;
    case V_CHAN: p = put_u(p, q->channel); break;
    case V_CC: p = q->cc_on ? put_u(p, q->cc_num) : put_s(p, "OFF"); break;
    case V_CCVAL: p = q->cc_mode ? put_u(p, q->cc_fixed) : put_s(p, "DYN"); break;
    case V_BLEED: p = q->spec_strict < .005f ? put_s(p, "OFF") : put_f(p, q->spec_strict, 2); break;
    case V_RANGE: p = q->spec_range < .005f ? put_s(p, "OFF") : put_f(p, q->spec_range, 2); break;
    case V_FLOOR: *p++ = '-'; p = put_u(p, (unsigned)(-q->floor_db + .5f)); p = put_s(p, "dB"); break;
    case V_PAD:
        if (!m->pad_on[k]) {
            p = put_s(p, m->pad_id[k] ? "OFF" : "NONE");
        } else {
            uint32_t id = m->pad_id[k];
            if ((id >> 16) & 0x1f) {
                *p++ = 'b';
                p = put_u(p, (id >> 16) & 0x1f);
            }
            *p++ = 'r';
            p = put_u(p, (id >> 8) & 0xf);
            *p++ = 'c';
            p = put_u(p, id & 0xf);
        }
        break;
    case V_PNOTE: p = put_note(p, m->pad_note[k]); *p++ = ' '; p = put_u(p, m->pad_note[k]); break;
    case V_LEARN: p = put_s(p, m->learn == k + 1 ? "HIT A PAD" : m->pad_id[k] ? "RELEARN" : "LEARN"); break;
    }
    *p = 0;
}

/* ---- values: one knob step */
static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

static void step_params(t2m_params *q, int v, int s)
{
    switch (v) {
    case V_SENS: q->sens = clampf(q->sens + (q->sens < 2.f ? .05f : .1f) * (float)s, .1f, 10.f); break;
    case V_THRESH: q->thresh = clampf(q->thresh + .25f * (float)s, 0.f, 31.f); break;
    case V_RETRIG: q->retrig = clampf(q->retrig + .25f * (float)s, 1.f, 16.f); break;
    case V_MASK: q->mask_ms = clampf(q->mask_ms + (float)s, 1.f, 64.f); break;      /* the patch's range is 15 .. 64; lower passes fast notes */
    case V_SCAN: q->scan_ms = clampf(q->scan_ms + .1f * (float)s, 0.f, 4.f); break;
    case V_STRICT: q->strict_db = clampf(q->strict_db + .25f * (float)s, 0.f, 20.f); break;
    case V_SPEED: q->speed_ms = clampf(q->speed_ms + .1f * (float)s, .2f, 10.f); break;
    case V_CURVE: q->curve = clampf(q->curve + .01f * (float)s, .1f, 1.5f); break;
    case V_NOTE: {                                    /* OFF, 0 .. 127 */
        int n = q->note_on ? q->note + 1 : 0;
        n = clampi(n + s, 0, 128);
        q->note_on = n > 0;
        if (n)
            q->note = (uint8_t)(n - 1);
        break;
    }
    case V_VEL: {                                     /* DYN, 1 .. 127 */
        int n = q->vel_mode ? q->vel_fixed : 0;
        n = clampi(n + s, 0, 127);
        q->vel_mode = n > 0;
        if (n)
            q->vel_fixed = (uint8_t)n;
        break;
    }
    case V_LEN: {                                     /* DYN, 5 .. 2000 ms */
        int n = q->len_mode ? q->len_ms : 0, st = n < 100 ? 5 : n < 500 ? 10 : 50;
        n = n + s * st;
        if (!q->len_mode && s > 0)
            n = 5;
        q->len_mode = n >= 5;
        if (n >= 5)
            q->len_ms = (uint16_t)clampi(n, 5, 2000);
        break;
    }
    case V_CHAN: q->channel = (uint8_t)clampi(q->channel + s, 1, 16); break;
    case V_CC: {                                      /* OFF, 0 .. 127 */
        int n = q->cc_on ? q->cc_num + 1 : 0;
        n = clampi(n + s, 0, 128);
        q->cc_on = n > 0;
        if (n)
            q->cc_num = (uint8_t)(n - 1);
        break;
    }
    case V_CCVAL: {
        int n = q->cc_mode ? q->cc_fixed : 0;
        n = clampi(n + s, 0, 127);
        q->cc_mode = n > 0;
        if (n)
            q->cc_fixed = (uint8_t)n;
        break;
    }
    case V_BLEED: q->spec_strict = clampf(q->spec_strict + .01f * (float)s, 0.f, .6f); break;
    case V_RANGE: q->spec_range = clampf(q->spec_range + .01f * (float)s, 0.f, 1.f); break;
    case V_FLOOR: q->floor_db = clampf(q->floor_db + (float)s, -120.f, -40.f); break;
    }
}

/* ---- geometry: w = page width, foot = the footer's line */
static int rows_top(int sheet)
{
    return sheet == SH_DETECT ? TOP + BAR_H + MET_H + 6 : TOP + BAR_H + 4;
}

static int row_h(int foot, int sheet)
{
    int h = (foot - rows_top(sheet) - 3) / 3;
    return h > 40 ? 40 : h;
}

static int cell_x(int w, int c) { return 4 + c * ((w - 8) / 4); }
static int cell_w(int w) { return (w - 8) / 4 - 3; }

/* switch row buttons: 0 IN L, 1 IN R, 2 ON, 3 TRS, 4 USB */
static const int16_t sw_x[5] = {4, 46, 104, 162, 204};
static const int16_t sw_w[5] = {38, 38, 40, 38, 38};

static float db_of(float x)                          /* 20 log10(x) for the meter, rough */
{
    union {
        float f;
        uint32_t u;
    } v = {x};
    float l2 = (float)((int)(v.u >> 23) - 127);
    v.u = (v.u & 0x7fffffu) | 0x3f800000u;
    l2 += (-0.34484843f * v.f + 2.02466578f) * v.f - 0.67487759f;
    return 6.0206f * l2;
}

static int meter_x(int len, float lin)               /* -48 .. 0 dB across len pixels */
{
    float db = lin > 1e-5f ? db_of(lin) : -100.f;
    return (int)(clampf((db + 48.f) / 48.f, 0.f, 1.f) * (float)len);
}

static int age_of(const struct t2m_mem *m, uint32_t blk) { return (int)(m->blocks - blk); }

static int vel_color(int v) { return v < 50 ? C_GREEN : v < 100 ? C_YELLOW : C_REC; }

static int lit(const struct t2m_mem *m, int k) { return m->hits[k] && age_of(m, m->hit_blk[k]) < LIT_BLOCKS; }

static void draw_meter(const struct t2m_mem *m, int k, int w, int d, int h)
{
    int mw = w - 8 - 64, lx = meter_x(mw, m->peak[k]);
    lp_text(4, d + (h - 8) / 2, k ? "R" : "L", C_GREY, 1);
    lp_frame(14, d, mw - 8, h, C_RAIL, 1);
    lp_box(15, d + 1, lx * (mw - 10) / (mw ? mw : 1), h - 2, lit(m, k) ? C_GREEN : k ? C_YELLOW : C_CYAN);
    float thr = 0.01f + m->p[k].thresh * (0.09f / 31.f);
    lp_vline(15 + meter_x(mw - 10, thr), d - 1, h + 2, C_RED);
}

/* the pads: lit by a hit, in the colour of its velocity */
static void draw_pads(const struct t2m_mem *m, int w, int d, int bh)
{
    int bw = (w - 12) / 2;
    for (int k = 0; k < T2M_INPUTS; k++) {
        int x = 4 + k * (bw + 4), on = m->on[k], l = on && lit(m, k), sel = (m->ui.in & 1) == k;
        int fc = on ? (k ? C_YELLOW : C_CYAN) : C_RAIL, ink = l ? C_BLACK : on ? C_WHITE : C_GREY;
        if (l)
            lp_box(x, d, bw, bh, vel_color(m->last_vel[k]));
        lp_frame(x, d, bw, bh, fc, sel ? 2 : 1);
        char b[24], *p = b;
        *p++ = k ? 'R' : 'L';
        *p = 0;
        lp_text(x + 5, d + 4, b, ink, 1);
        p = b;
        if (!on)
            p = put_s(p, "OFF");
        else if (!m->hits[k])
            p = put_s(p, "READY");
        else if (m->p[k].note_on)
            p = put_note(p, m->last_note[k]);
        else
            p = put_s(p, "HIT");
        *p = 0;
        lp_text_c(x, d + 8, bw, b, ink, 2);
        p = b;
        if (on && m->hits[k]) {
            p = put_s(p, "VEL ");
            p = put_u(p, m->last_vel[k]);
            p = put_s(p, "  #");
            p = put_u(p, m->hits[k]);
        } else if (on) {
            p = put_s(p, "WAITING FOR A HIT");
        }
        *p = 0;
        lp_text_c(x, d + bh - 14, bw, b, ink, 1);
    }
}

/* the last 4 s, one lane per input, newest at the right, bar height = velocity */
static void draw_hist(const struct t2m_mem *m, int w, int d0, int d1)
{
    int lane_h = (d1 - d0 - 3) / 2, lw = w - 8 - 10;
    for (int k = 0; k < T2M_INPUTS; k++) {
        int top = d0 + k * (lane_h + 3), c = k ? C_YELLOW : C_CYAN;
        lp_text(4, top + lane_h / 2 - 4, k ? "R" : "L", C_GREY, 1);
        lp_frame(14, top, lw, lane_h, C_RAIL, 1);
        for (int i = 0; i < T2M_HIST; i++) {
            const struct t2m_hit *h = &m->hist[k][i];
            int age = age_of(m, h->blk);
            if (!h->vel || age < 0 || age >= T2M_HIST_BLOCKS)
                continue;
            int x = 15 + (lw - 5) - age * (lw - 6) / T2M_HIST_BLOCKS;
            int bh = 1 + h->vel * (lane_h - 3) / 127;
            lp_box(x, top + lane_h - 1 - bh, 3, bh, age < LIT_BLOCKS ? C_WHITE : c);
        }
    }
    lp_text(14, d1 + 1, "-4s", C_GREY, 1);
    lp_text(w - 8 - 24, d1 + 1, "now", C_GREY, 1);
}

/* ---- the CLK sheet */
static char *put_us(char *p, float v)                /* microseconds, whole */
{
    return put_u(p, (unsigned)(v + .5f > 99999.f ? 99999.f : v + .5f));
}

static void clk_col(const struct clkmon *c, int src, int x, int d0, int st)
{
    struct clk_stats o;
    clkmon_stats(c, src, &o);
    char b[40], *p;
    int has = o.n > 0, has_stk = c->r[src].n_stk > 0;
    p = put_s(b, src ? "USB F8 " : "TRS F8 ");
    p = put_u(p, o.f8);
    *p = 0;
    lp_text(x, d0, b, o.f8 ? C_CYAN : C_GREY, 1);
    p = put_s(b, "BPM ");
    p = put_f(p, o.bpm, 1);
    p = put_s(p, " FA");
    p = put_u(p, o.fa);
    p = put_s(p, " FB");
    p = put_u(p, o.fb);
    p = put_s(p, " FC");
    p = put_u(p, o.fc);
    *p = 0;
    lp_text(x, d0 + st, b, has ? C_LIGHT : C_GREY, 1);
    p = put_s(b, "ARR ");
    p = put_us(p, o.arr_min);
    *p++ = '-';
    p = put_us(p, o.arr_max);
    p = put_s(p, " sd");
    p = put_us(p, o.arr_sd);
    *p = 0;
    lp_text(x, d0 + 2 * st, b, has ? C_GREEN : C_GREY, 1);
    p = put_s(b, "STK ");
    p = put_us(p, o.stk_min);
    *p++ = '-';
    p = put_us(p, o.stk_max);
    p = put_s(p, " sd");
    p = put_us(p, o.stk_sd);
    *p = 0;
    lp_text(x, d0 + 3 * st, b, has_stk ? C_YELLOW : C_GREY, 1);
    p = put_s(b, "LAT ");
    p = put_us(p, o.lat_avg);
    p = put_s(p, " max ");
    p = put_us(p, o.lat_max);
    *p = 0;
    lp_text(x, d0 + 4 * st, b, has ? C_LIGHT : C_GREY, 1);
}

static void draw_clk(struct t2m_mem *m, int w, int foot)
{
    const struct clkmon *c = &m->clk;
    char b[48], *p;
    lp_text(6, 3, "CLK", C_CYAN, 1);
    lp_text(36, 3, "MIDI CLOCK IN, US", C_GREY, 1);
    int d0 = TOP + 3, st = (foot - d0 - 12) / 10;
    st = st > 14 ? 14 : st < 8 ? 8 : st;
    clk_col(c, 0, 4, d0, st);
    clk_col(c, 1, w / 2 + 2, d0, st);
    lp_vline(w / 2 - 2, TOP, 5 * st, C_RAIL);
    float mhz = clkmon_mhz(c);
    struct clk_stats a, u;
    clkmon_stats(c, 0, &a);
    clkmon_stats(c, 1, &u);
    p = put_s(b, "CORE ");
    if (mhz > 0.f)
        p = put_f(p, mhz, 0);
    else
        p = put_s(p, "..");
    p = put_s(p, "MHZ  ISR ");
    p = put_u(p, a.cost);
    *p++ = '/';
    p = put_u(p, u.cost);
    p = put_s(p, "CY  LOST ");
    p = put_u(p, a.over + u.over);
    p = put_s(p, "  TAP=RESET");
    *p = 0;
    lp_text(4, d0 + 5 * st, b, C_GREY, 1);
    struct clk_xp_stats xp;
    clkmon_xp_stats(c, &xp);
    p = put_s(b, "BLK ");
    p = put_f(p, clkmon_blk_ms(c, 0), 2);
    *p++ = '-';
    p = put_f(p, clkmon_blk_ms(c, 1), 2);
    p = put_s(p, "MS  FS ");
    if (xp.fs > 0.f)
        p = put_u(p, (unsigned)(xp.fs + .5f));
    else
        p = put_s(p, "..");
    p = put_s(p, "HZ");
    *p = 0;
    lp_text(4, d0 + 6 * st, b, C_GREY, 1);
    p = put_s(b, "SEQ BPM ");
    p = put_f(p, xp.bpm, 1);
    p = put_s(p, "  TICKS/BLK ");
    p = put_u(p, (unsigned)xp.dt_min);
    *p++ = '-';
    p = put_u(p, (unsigned)xp.dt_max);
    p = put_s(p, "  N ");
    p = put_u(p, xp.calls);
    *p = 0;
    lp_text(4, d0 + 7 * st + 2, b, xp.calls ? C_LIGHT : C_GREY, 1);
    p = put_s(b, "SEQ OFF-LINE ");
    p = put_s(p, xp.res_min < 0.f ? "-" : "+");
    p = put_us(p, xp.res_min < 0.f ? -xp.res_min : xp.res_min);
    p = put_s(p, "..");
    p = put_s(p, xp.res_max < 0.f ? "-" : "+");
    p = put_us(p, xp.res_max < 0.f ? -xp.res_max : xp.res_max);
    p = put_s(p, " sd");
    p = put_us(p, xp.res_sd);
    p = put_s(p, " US");
    *p = 0;
    lp_text(4, d0 + 8 * st + 2, b, xp.calls ? C_YELLOW : C_GREY, 1);
    lp_text(4, d0 + 9 * st + 4, "SEQ = stock sequencer position vs audio time", C_GREY, 1);
}

void t2m_page_draw(int w, int foot)
{
    struct t2m_mem *m = t2m_mem();
    lp_box(1, 1, w - 2, TOP - 2, C_BG);
    lp_box(1, TOP, w - 2, foot - TOP, C_BG);
    if (!m) {
        lp_text(6, 3, "TRIG", C_CYAN, 1);
        lp_text(6, TOP + 10, "NO MEMORY FOR TRIGGER2MIDI THIS BOOT", C_REC, 1);
        return;
    }
    int k = UI->in & 1, sheet = UI->sheet < SHEETS ? UI->sheet : 0;
    char b[48], *p;
    if (sheet == SH_CLK) {
        draw_clk(m, w, foot);
        return;
    }

    /* top bar: both inputs at a glance */
    lp_text(6, 3, "TRIG", C_CYAN, 1);
    for (int i = 0; i < T2M_INPUTS; i++) {
        p = b;
        *p++ = i ? 'R' : 'L';
        *p++ = ' ';
        if (m->on[i]) {
            p = put_u(p, m->hits[i]);
            p = put_s(p, " v");
            p = put_u(p, m->last_vel[i]);
        } else {
            p = put_s(p, "off");
        }
        *p = 0;
        lp_text(48 + i * 120, 3, b, m->on[i] ? C_LIGHT : C_GREY, 1);
    }

    /* switch row (every sheet) */
    int d = TOP + 2;
    static const char *const sw[5] = {"IN L", "IN R", 0, "TRS", "USB"};
    for (int i = 0; i < 5; i++) {
        int on = i < 2 ? k == i : i == 2 ? m->on[k] : i == 3 ? (m->port[k] & T2M_PORT_TRS) : (m->port[k] & T2M_PORT_USB);
        const char *s = i == 2 ? (m->on[k] ? "ON" : "OFF") : sw[i];
        int c = i == 2 && !on ? C_RED : on ? C_CYAN : C_RAIL;
        lp_frame(sw_x[i], d, sw_w[i], BAR_H - 4, c, 1);
        lp_text_c(sw_x[i], d + 4, sw_w[i], s, on ? C_WHITE : C_GREY, 1);
    }
    lp_text(sw_x[4] + sw_w[4] + 8, d + 4, "PORT", C_GREY, 1);

    if (sheet == SH_HITS) {
        int pd = TOP + BAR_H + 2, ph = 50;
        draw_pads(m, w, pd, ph);
        int md = pd + ph + 5;
        draw_meter(m, 0, w, md, 9);
        draw_meter(m, 1, w, md + 12, 9);
        p = put_s(b, "SENS ");
        p = put_f(p, m->p[k].sens, 2);
        *p = 0;
        lp_text(w - 60, md + 1, b, C_LIGHT, 1);
        p = put_s(b, "THR ");
        p = put_f(p, m->p[k].thresh, 2);
        *p = 0;
        lp_text(w - 60, md + 13, b, C_LIGHT, 1);
        draw_hist(m, w, md + 26, foot - 12);
        return;
    }

    if (sheet == SH_DETECT)
        draw_meter(m, k, w, TOP + BAR_H + 3, MET_H - 2);
    int rh = row_h(foot, sheet), cw = cell_w(w);
    for (int r = 0; r < 3; r++) {
        int rd = rows_top(sheet) + r * rh, sel = UI->row == r;
        for (int c = 0; c < 4; c++) {
            int v = sheet_vals[sheet - 1][r * 4 + c], x = cell_x(w, c);
            if (v == V_NONE)
                continue;
            lp_frame(x, rd, cw, rh - 3, sel ? C_PINK : C_RAIL, 1);
            lp_text_c(x, rd + 3, cw, val_name[v], sel ? C_LIGHT : C_GREY, 1);
            value_text(m, k, v, b);
            int armed = v == V_LEARN && m->learn == k + 1;
            lp_text_c(x, rd + 3 + (rh - 3 > 26 ? 14 : 10), cw, b, armed ? C_YELLOW : sel ? C_WHITE : C_LIGHT, 1);
        }
    }
    if (sheet == SH_OUT) {
        int td = rows_top(sheet) + 3 * rh + 2;
        p = put_s(b, "PAD TRIGGERS SENT ");
        p = put_u(p, m->pad_sent[k]);
        *p = 0;
        lp_text(6, td, b, C_GREY, 1);
    }
}

/* A touch at x / d (page coordinates) above the footer. */
void t2m_page_touch(int x, int d, int w, int foot)
{
    struct t2m_mem *m = t2m_mem();
    if (!m)
        return;
    int k = UI->in & 1, sheet = UI->sheet < SHEETS ? UI->sheet : 0;
    if (sheet == SH_CLK) {                            /* anywhere on the CLK sheet: start the numbers again */
        m->clk.reset = 1;
        return;
    }
    if (d >= TOP && d < TOP + BAR_H) {
        for (int i = 0; i < 5; i++) {
            if (x < sw_x[i] || x >= sw_x[i] + sw_w[i])
                continue;
            if (i < 2)
                UI->in = (uint8_t)i;
            else if (i == 2)
                m->on[k] = !m->on[k];
            else
                m->port[k] ^= (uint8_t)(i == 3 ? T2M_PORT_TRS : T2M_PORT_USB);
        }
        return;
    }
    if (sheet == SH_HITS) {                           /* a tap on a pad shows that input */
        int pd = TOP + BAR_H + 2;
        if (d >= pd && d < pd + 50)
            UI->in = (uint8_t)(x >= w / 2 ? 1 : 0);
        return;
    }
    for (int r = 0; r < 3; r++) {
        int rd = rows_top(sheet) + r * row_h(foot, sheet);
        if (d < rd || d >= rd + row_h(foot, sheet) - 3 || x < 4 || x >= w - 4)
            continue;
        UI->row = (uint8_t)r;
        int c = (x - 4) / ((w - 8) / 4);
        if (c < 4 && sheet_vals[sheet - 1][r * 4 + c] == V_LEARN)
            m->learn = m->learn ? 0 : (uint8_t)(k + 1);   /* LEARN: the next stock pad hit is this input's target */
    }
}

void t2m_page_sheet(int sheet)
{
    struct t2m_mem *m = t2m_mem();
    if (m && sheet >= 0 && sheet < SHEETS) {
        UI->sheet = (uint8_t)sheet;
        m->learn = 0;
    }
}

int t2m_page_sheet_now(void)
{
    struct t2m_mem *m = t2m_mem();
    return m && UI->sheet < SHEETS ? UI->sheet : 0;
}

/* Knob 0..3 (the Looper page's order: track 1 .. 4) turns the shown row's value 0..3. */
void t2m_page_knob(int knob, int counts)
{
    struct t2m_mem *m = t2m_mem();
    if (!m || knob < 0 || knob > 3 || UI->sheet == SH_CLK)
        return;
    int a = UI->kacc[knob] + counts, steps = a / STEP_COUNTS;
    UI->kacc[knob] = (int16_t)(a - steps * STEP_COUNTS);
    if (!steps)
        return;
    int k = UI->in & 1, sheet = UI->sheet < SHEETS ? UI->sheet : 0;
    int v = sheet == SH_HITS ? sheet_vals[0][knob] : sheet_vals[sheet - 1][(UI->row % 3) * 4 + knob];
    if (v == V_NONE || v == V_LEARN)
        return;
    if (v == V_PAD) {                                 /* off / on (on only once a pad has been learned) */
        m->pad_on[k] = steps > 0 && m->pad_id[k] ? 1 : 0;
    } else if (v == V_PNOTE) {
        m->pad_note[k] = (uint8_t)clampi((int)m->pad_note[k] + steps, 0, 127);
    } else {
        t2m_params q = m->p[k];                       /* edit a copy: the audio task never sees half a change */
        uint8_t was_fixed = q.vel_mode;
        step_params(&q, v, steps);
        if (v == V_VEL && !was_fixed && q.vel_mode) {         /* fixed velocity needs no peak search: SCAN 0 (kept to put back) */
            if (q.scan_ms > 0.f)
                m->scan_saved[k] = q.scan_ms;
            q.scan_ms = 0.f;
        } else if (v == V_VEL && was_fixed && !q.vel_mode && q.scan_ms == 0.f) {
            q.scan_ms = m->scan_saved[k];                      /* back to DYN: SCAN as it was, unless it was changed meanwhile */
        }
        m->p[k] = q;
    }
}

/* For the page's redraw test: what the page shows. */
uint32_t t2m_page_sig(void)
{
    struct t2m_mem *m = t2m_mem();
    if (!m)
        return 0;
    if (m->ui.sheet == SH_CLK)
        return clkmon_sig(&m->clk) ^ 0xc1c00001u;
    int k = m->ui.in & 1;
    uint32_t h = 2166136261u;
    const uint8_t *b = (const uint8_t *)&m->p[k];
    for (unsigned i = 0; i < sizeof m->p[k]; i++)
        h = (h ^ b[i]) * 16777619u;
    for (int i = 0; i < T2M_INPUTS; i++) {
        float db = m->peak[i] > 1e-5f ? db_of(m->peak[i]) : -100.f;
        h = (h ^ (uint32_t)(int)(db * 0.5f)) * 16777619u;             /* 2 dB steps of the meters */
        h = (h ^ (m->hits[i] + (uint32_t)lit(m, i) * 977u + m->pad_sent[i] * 31u)) * 16777619u;
        h = (h ^ ((uint32_t)m->pad_id[i] + m->pad_note[i] + m->pad_on[i] * 7u)) * 16777619u;
    }
    h = (h ^ (uint32_t)(m->on[0] | m->on[1] << 1 | m->port[0] << 2 | m->port[1] << 4 | m->ui.in << 6 | m->ui.row << 7 |
                        m->ui.sheet << 9 | m->learn << 11)) * 16777619u;
    if (m->ui.sheet == SH_HITS) {                                     /* the history scrolls while it holds a hit */
        int fresh = 0;
        for (int i = 0; i < T2M_INPUTS; i++)
            fresh |= m->hits[i] && age_of(m, m->hit_blk[i]) < T2M_HIST_BLOCKS + 16;
        if (fresh)
            h = (h ^ (m->blocks >> 4)) * 16777619u;
    }
    return h;
}
