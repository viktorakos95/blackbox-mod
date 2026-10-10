/*
 * Trigger2MIDI page: the TRIG tab of the Looper page (src/looper_page.c draws the frame, the footer and hands over the
 * area between the top bar and the footer, the touches there and the four knobs).
 *
 *   top bar   TRIG  L <hits> v<last>   R <hits> v<last>
 *   row       [IN L] [IN R]   [ON]   [TRS] [USB]          the input being edited, its switch, its output ports
 *   meter     the input level after SENS (-48..0 dB), the threshold as a red tick, green for a moment on each hit
 *   4 rows    DETECT  SENS  THRESH  RETRIG  MASK          a tap on a row hands it to the four knobs (pink frame):
 *             ONSET   SCAN  STRICT  SPEED   CURVE         knob n turns the row's n-th value (the knobs' order is the
 *             NOTE    NOTE  VEL     LEN     CHAN          tracks' order on the Looper page: 1 2 3 4 left to right)
 *             CC      CC    CC VAL  BLEED   RANGE
 * NOTE OFF = no note (the patch's NO-NOTE); CC OFF = no CC; VEL / CC VAL / LEN below their lowest value = DYN.
 * Knobs: 40 counts a step (the SAMPLR envelope's rate).
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
#define C_RAIL   0x10
#define C_GREY   0x09
#define C_LIGHT  0x16
#define C_WHITE  0x0f
#define C_GREEN  0x0b
#define C_RED    0x0c
#define C_YELLOW 0x14
#define C_CYAN   0x1b
#define C_PINK   0x20

#define TOP   16                 /* first line below the top bar */
#define BAR_H 20                 /* the switch row */
#define MET_H 12                 /* the meter */
#define STEP_COUNTS 40
#define FLASH_BLOCKS 24          /* the meter is green this long after a hit (128 ms) */

enum { V_SENS, V_THRESH, V_RETRIG, V_MASK, V_SCAN, V_STRICT, V_SPEED, V_CURVE, V_NOTE, V_VEL, V_LEN, V_CHAN, V_CC, V_CCVAL,
       V_BLEED, V_RANGE, VALUES };
static const char *const val_name[VALUES] = {"SENS", "THRESH", "RETRIG", "MASK", "SCAN", "STRICT", "SPEED", "CURVE",
                                             "NOTE", "VEL", "LEN", "CHAN", "CC", "CC VAL", "BLEED", "RANGE"};

/* page state: in the SDRAM block (t2m_bb.h struct t2m_mem's ui) */
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

static void value_text(const t2m_params *q, int v, char *b)
{
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
    }
    *p = 0;
}

/* ---- values: one knob step */
static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

static void step_value(t2m_params *q, int v, int s)
{
    switch (v) {
    case V_SENS: q->sens = clampf(q->sens + (q->sens < 2.f ? .05f : .1f) * (float)s, .1f, 10.f); break;
    case V_THRESH: q->thresh = clampf(q->thresh + .25f * (float)s, 0.f, 31.f); break;
    case V_RETRIG: q->retrig = clampf(q->retrig + .25f * (float)s, 1.f, 16.f); break;
    case V_MASK: q->mask_ms = clampf(q->mask_ms + (float)s, 15.f, 64.f); break;
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
    }
}

/* ---- geometry: w = page width, foot = the footer's line */
static int row_h(int foot)
{
    int h = (foot - (TOP + BAR_H + MET_H + 6)) / 4;
    return h > 34 ? 34 : h;
}

static int row_d(int foot, int r)
{
    return TOP + BAR_H + MET_H + 6 + r * row_h(foot);
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

static int meter_x(int w, float lin)                 /* -48 .. 0 dB across the meter */
{
    float db = lin > 1e-5f ? db_of(lin) : -100.f;
    float f = clampf((db + 48.f) / 48.f, 0.f, 1.f);
    return (int)(f * (float)(w - 8 - 64));
}

void t2m_page_draw(int w, int foot)
{
    struct t2m_mem *m = t2m_mem();
    lp_box(1, 1, w - 2, TOP - 2, C_BG);
    lp_box(1, TOP, w - 2, foot - TOP, C_BG);
    if (!m) {
        lp_text(6, 3, "TRIG", C_CYAN, 1);
        lp_text(6, TOP + 10, "NO MEMORY FOR TRIGGER2MIDI THIS BOOT", C_RED, 1);
        return;
    }
    int k = UI->in & 1;
    t2m_params *q = &m->p[k];
    char b[48], *p;

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

    /* switch row */
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

    /* meter */
    d = TOP + BAR_H + 2;
    int mw = w - 8 - 64, lit = m->blocks - m->hit_blk[k] < FLASH_BLOCKS && m->hits[k];
    lp_frame(4, d, mw + 2, MET_H - 2, C_RAIL, 1);
    int lx = meter_x(w, m->peak[k]);
    lp_box(5, d + 1, lx, MET_H - 4, lit ? C_GREEN : C_YELLOW);
    float thr = 0.01f + q->thresh * (0.09f / 31.f);
    int tx = meter_x(w, thr);
    lp_vline(5 + tx, d - 1, MET_H, C_RED);
    p = put_s(b, "HIT ");
    p = put_u(p, m->hits[k]);
    *p = 0;
    lp_text(w - 60, d + 1, b, m->on[k] ? C_LIGHT : C_GREY, 1);

    /* the four rows */
    int rh = row_h(foot), cw = cell_w(w);
    for (int r = 0; r < 4; r++) {
        int rd = row_d(foot, r), sel = UI->row == r;
        for (int c = 0; c < 4; c++) {
            int v = r * 4 + c, x = cell_x(w, c);
            lp_frame(x, rd, cw, rh - 3, sel ? C_PINK : C_RAIL, 1);
            lp_text_c(x, rd + 3, cw, val_name[v], sel ? C_LIGHT : C_GREY, 1);
            value_text(q, v, b);
            lp_text_c(x, rd + 3 + (rh - 3 > 26 ? 12 : 10), cw, b, sel ? C_WHITE : C_LIGHT, 1);
        }
    }
}

/* A touch at x / d (page coordinates) above the footer. */
void t2m_page_touch(int x, int d, int w, int foot)
{
    struct t2m_mem *m = t2m_mem();
    if (!m)
        return;
    int k = UI->in & 1;
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
    for (int r = 0; r < 4; r++) {
        int rd = row_d(foot, r);
        if (d >= rd && d < rd + row_h(foot) - 3 && x >= 4 && x < w - 4)
            UI->row = (uint8_t)r;
    }
}

/* Knob 0..3 (the Looper page's order: track 1 .. 4) turns the selected row's value 0..3. */
void t2m_page_knob(int knob, int counts)
{
    struct t2m_mem *m = t2m_mem();
    if (!m || knob < 0 || knob > 3)
        return;
    int a = UI->kacc[knob] + counts, steps = a / STEP_COUNTS;
    UI->kacc[knob] = (int16_t)(a - steps * STEP_COUNTS);
    if (!steps)
        return;
    t2m_params q = m->p[UI->in & 1];                 /* edit a copy: the audio task never sees half a change */
    step_value(&q, (UI->row & 3) * 4 + knob, steps);
    m->p[UI->in & 1] = q;
}

/* For the page's redraw test: what the page shows. */
uint32_t t2m_page_sig(void)
{
    struct t2m_mem *m = t2m_mem();
    if (!m)
        return 0;
    int k = m->ui.in & 1;
    float db = m->peak[k] > 1e-5f ? db_of(m->peak[k]) : -100.f;
    uint32_t h = 2166136261u;
    const uint8_t *b = (const uint8_t *)&m->p[k];
    for (unsigned i = 0; i < sizeof m->p[k]; i++)
        h = (h ^ b[i]) * 16777619u;
    h = (h ^ (uint32_t)(int)(db * 0.5f)) * 16777619u;             /* 2 dB steps of the meter */
    h = (h ^ (m->hits[0] + 7u * m->hits[1])) * 16777619u;
    h = (h ^ (uint32_t)(m->on[0] | m->on[1] << 1 | m->port[0] << 2 | m->port[1] << 4 | m->ui.in << 6 | m->ui.row << 7 |
                        (uint32_t)(m->blocks - m->hit_blk[k] < FLASH_BLOCKS && m->hits[k]) << 9)) * 16777619u;
    return h;
}
