/*
 * LIRA-8 engine, see lira.h. Port of the signal flow of LIRA-8_Pd_Standalone/_LIRA-8.pd and its abstractions
 * (lira.voice, os.triangle~, prm.*, compressor~, expander~, ma.tanh~) by Mike Moreno DSP.
 *
 * Differences from Pd, all on purpose:
 *  - per-sample processing instead of 64-sample blocks, so the voice-to-voice "delay buses", the master feedback and the
 *    delay loops are one sample late rather than one block late (delay lines keep Pd's 64-sample minimum);
 *  - line~ ramps are exact per-sample ramps; the delay taps use linear interpolation (Pd: 4-point);
 *  - expander level is a 512-sample rectangular-window mean square (Pd: env~ with a Hann window);
 *  - no libm: small polynomial exp2/log2/cos;
 *  - for the Cortex-M7's budget everything slow runs once per LIRA_CR-sample block: all line~ ramps (they step every
 *    LIRA_CR samples), the hyper-LFO, vibrato, touch envelopes, the oscillators' low-pass coefficients (from the
 *    unmodulated frequency) and the expander gain. Oscillators, cross-FM, delays and saturation run per sample.
 * pow~ 31 in the master (Pd) is taken as positive-only (0 for negative input).
 */
#include "lira.h"

#pragma GCC optimize("O2", "no-tree-loop-distribute-patterns")   /* no libc to call */

#define TWO_PI 6.2831853f
#define LIRA_CR 16          /* control block, samples */

static inline float fabs_(float x) { return x < 0.f ? -x : x; }
static inline float fmin_(float a, float b) { return a < b ? a : b; }
static inline float fmax_(float a, float b) { return a > b ? a : b; }
static inline float clip_(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float floor_(float x) { int i = (int)x; return (float)(i - ((float)i > x)); }

static float fexp2(float x)
{
    if (x < -126.f)
        return 0.f;
    if (x > 126.f)
        x = 126.f;
    float fl = floor_(x), f = x - fl;
    float p = 1.f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    union { float f; uint32_t u; } v;
    v.u = (uint32_t)((int)fl + 127) << 23;
    return p * v.f;
}

static float flog2(float x)
{
    union { float f; uint32_t u; } v;
    v.f = x;
    float e = (float)((int)((v.u >> 23) & 255) - 127);
    v.u = (v.u & 0x7fffffu) | 0x3f800000u;
    float m = v.f;
    return e + (-1.7417939f + (2.8212026f + (-1.4699568f + (0.44717955f - 0.056570851f * m) * m) * m) * m);
}

static float fpow(float b, float e) { return b > 0.f ? fexp2(e * flog2(b)) : 0.f; }

static float fcos(float x) /* cos(2*pi*x), x in cycles */
{
    float t = x + 0.25f;
    t -= floor_(t + 0.5f);
    float y = 8.f * t * (1.f - 2.f * fabs_(t));
    return y * (0.775f + 0.225f * fabs_(y));
}

static float mtof(float m) { return m <= -1500.f ? 0.f : 440.f * fexp2((m - 69.f) * (1.f / 12.f)); }
static float ftom(float f) { return f <= 0.f ? -1500.f : 69.f + 12.f * flog2(f * (1.f / 440.f)); }
static float db2lin(float db) { return fexp2(db * 0.16609640f); }

/* Pade tanh, as ma.tanh~ */
static float ptanh(float x)
{
    x = clip_(x, -3.f, 3.f);
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

static float wrap1(float x) { return x - floor_(x); }

/* ---- line~ ---- */
static void rjump(lira_ramp *r, float v) { r->cur = r->tgt = v; r->n = 0; r->step = 0.f; }
static void rset(lira_ramp *r, float tgt, int n)
{
    r->tgt = tgt;
    if (n < 1) {
        r->cur = tgt;
        r->n = 0;
        return;
    }
    r->n = n;
    r->step = (tgt - r->cur) / (float)n;
}
static void rstep(lira_ramp *r, int m)    /* advance a ramp by m samples at once */
{
    if (!r->n)
        return;
    if (r->n <= m) {
        r->cur = r->tgt;
        r->n = 0;
    } else {
        r->cur += r->step * (float)m;
        r->n -= m;
    }
}

static float rnd(lira_t *l)    /* -1 .. 1 */
{
    l->rng ^= l->rng << 13;
    l->rng ^= l->rng >> 17;
    l->rng ^= l->rng << 5;
    return (float)(int32_t)l->rng * (1.f / 2147483648.f);
}

/* ---- Pd hip~ / lop~ ---- */
static float hip(float *last, float x, float coef)
{
    float nw = x + coef * *last, y = nw - *last;
    *last = nw;
    return y;
}
static float lop(float *y, float x, float coef)
{
    *y += coef * (x - *y);
    return *y;
}

/* ---- os.triangle~: PolyBLEP saw pair -> square, one-pole low-pass -> triangle ---- */
static float blep(float t, float dt)
{
    float r = 0.f, u;
    if (t <= dt) {
        u = t / dt - 1.f;
        r = u * u;
    }
    if (t >= 1.f - dt) {
        u = (t - 1.f) / dt + 1.f;
        r -= u * u;
    }
    return r;
}

/* one sample; o->a (the low-pass coefficient) is set at control rate by the caller */
static inline float osc_tick(lira_osc *o, float f, float pw, float isr, float *tri)
{
    float dt = fabs_(f) * isr;
    if (dt < 1e-7f)
        dt = 1e-7f;
    float ph = o->ph + dt;
    if (ph >= 1.f) {
        ph -= floor_(ph);
        o->pwh = clip_(pw, 0.f, 1.f);       /* samphold~: sampled when the phasor wraps */
    }
    o->ph = ph;
    float p2 = ph + o->pwh;
    if (p2 >= 1.f)
        p2 -= 1.f;
    float s = 2.f * (ph - p2) + blep(ph, dt) - blep(p2, dt) + 2.f * o->pwh - 1.f;
    o->lp = s + o->a * (o->lp - s);
    *tri = o->lp * 2.65f - 0.145f;
    return s * 0.59f;
}

static float lp_coef(float f, float sr)  /* rpole~ coefficient of os.triangle~'s low-pass at max(|f| / 4, 100) Hz */
{
    float fc = clip_(fmax_(fabs_(f) * 0.25f, 100.f), 0.f, sr * 0.5f);
    return fexp2(-TWO_PI * 1.44269504f * fc / sr);
}

/* ---- compressor~ (attack/release 2.5 ms, threshold -12 dB, ratio 5) ---- */
static float comp_run(lira_comp *c, float x)
{
    float a = fabs_(x), k = a > c->env ? c->atk : c->rel;
    c->env = a * (1.f - k) + k * c->env;
    float g = c->env > 1e-9f ? ((c->env - 0.25118864f) * 0.2f + 0.25118864f) / c->env : 0.f;
    return x * ptanh(clip_(g, 0.f, 1.f));
}

/* ---- expander~ (threshold -60 dB, ratio 5, 11 ms gain smoothing) ---- */
static float exp_run(lira_exp *e, float x, int n11)
{
    e->acc += x * x;
    if (++e->cnt >= 256) {
        float p = (e->acc + e->prev) * (1.f / 512.f);
        e->prev = e->acc;
        e->acc = 0.f;
        e->cnt = 0;
        float db = p > 1e-10f ? 3.0103f * flog2(p) : -100.f;
        float red = 0.8f * fmin_(db + 60.f, 0.f);
        if (!e->init) {
            rjump(&e->red, red);
            e->init = 1;
        }
        rset(&e->red, red, n11);
    }
    e->g += 0.0625f * (e->gt - e->g);       /* gt: 10^(red/20), set once per control block */
    return x * e->g;
}

/* ---- helpers ---- */
static void retune(lira_t *l, int o)
{
    static const float lo[8] = {-16.f, -16.f, 7.f, 9.f, 20.f, 20.f, 33.f, 33.f};
    static const float hi[8] = {93.f, 93.f, 109.f, 107.f, 116.54f, 116.54f, 126.24f, 131.22f};
    float p = l->params[LIRA_TUNE1 + o] * (1.f / 127.f);
    float pitch = l->params[o < 4 ? LIRA_PITCH1234 : LIRA_PITCH5678] * (1.f / 127.f) * 1.99f + 0.01f;
    float f = mtof(lo[o] + p * (hi[o] - lo[o])) * pitch;
    if (l->params[LIRA_QUANTIZE] > 0.5f)
        f = mtof(floor_(ftom(f) + 0.5f));
    rset(&l->v[o >> 1].tune[o & 1], f, l->n23);
}

static void trigger(lira_t *l, int s)
{
    int v = s >> 1;
    int fast = l->params[LIRA_FAST12 + v] > 0.5f;
    int on = l->v[v].key[s & 1];
    float att = fast ? 100.f : 200.f, rel = fast ? 100.f : 8000.f;
    rset(&l->v[v].env[s & 1], on ? 1.f : 0.f, (int)((on ? att : rel) * 0.001f * l->sr));
}

void lira_param(lira_t *l, int id, float x)
{
    l->params[id] = x;
    float d = x * (1.f / 127.f);
    int n = l->n23;
    switch (id) {
    case LIRA_TUNE1: case LIRA_TUNE2: case LIRA_TUNE3: case LIRA_TUNE4:
    case LIRA_TUNE5: case LIRA_TUNE6: case LIRA_TUNE7: case LIRA_TUNE8:
        retune(l, id - LIRA_TUNE1);
        break;
    case LIRA_PITCH1234: case LIRA_PITCH5678: case LIRA_QUANTIZE: {
        int a = id == LIRA_PITCH5678 ? 4 : 0, b = id == LIRA_PITCH1234 ? 4 : 8;
        for (int o = a; o < b; o++)
            retune(l, o);
        break;
    }
    case LIRA_FAST12: case LIRA_FAST34: case LIRA_FAST56: case LIRA_FAST78:
        trigger(l, (id - LIRA_FAST12) * 2);
        trigger(l, (id - LIRA_FAST12) * 2 + 1);
        break;
    case LIRA_SHARP12: case LIRA_SHARP34: case LIRA_SHARP56: case LIRA_SHARP78:
        rset(&l->v[id - LIRA_SHARP12].sharp, d * d, n);
        break;
    case LIRA_MOD12: case LIRA_MOD34: case LIRA_MOD56: case LIRA_MOD78:
        rset(&l->v[id - LIRA_MOD12].mod, 2.f * d * d * d * d, n);
        break;
    case LIRA_HOLD1234: case LIRA_HOLD5678:
        rset(&l->hold[id - LIRA_HOLD1234], d * d, n);
        break;
    case LIRA_VIBRATO:
        for (int v = 0; v < 4; v++)
            rset(&l->v[v].vib, x, n);
        break;
    case LIRA_DRV: {
        rset(&l->drv_g, db2lin(3.f + fpow(3.f, 2.f * d + 1.f)), n);
        break;
    }
    case LIRA_DST_MIX: rset(&l->dst, d, n); break;
    case LIRA_VOL: rset(&l->vol, d * d, n); break;
    case LIRA_F_A: rset(&l->lfo_a, mtof(fpow(d, 2.f) * 127.f - 75.f), n); break;
    case LIRA_F_B: rset(&l->lfo_b, mtof(fpow(d, 2.f) * 127.f - 75.f), n); break;
    case LIRA_TIME1: case LIRA_TIME2: {
        lira_line *ln = &l->line[id - LIRA_TIME1];
        float ms = 1.45125f * fexp2(12.f * d);
        float mx = (float)(ln->len - 4) * 1000.f / l->sr;
        rset(&ln->time, fmin_(ms, mx), n);
        break;
    }
    case LIRA_FEEDBACK:
        rset(&l->fbline, fexp2(2.f * d) * d, n);
        break;
    case LIRA_DEL_MIX: rset(&l->dmix, d, n); break;
    case LIRA_MOD1: case LIRA_MOD2:
        rset(&l->line[id - LIRA_MOD1].mod, 10.f * d * d, n);
        break;
    default:
        break;
    }
}

void lira_key(lira_t *l, int s, int on)
{
    l->v[s >> 1].key[s & 1] = on != 0;
    trigger(l, s);
}

int lira_size(void) { return (int)sizeof(lira_t); }

void lira_reset(lira_t *l, float sr, float *dl1, float *dl2, int len, uint32_t seed)
{
    char *p = (char *)l;
    for (unsigned i = 0; i < sizeof *l; i++)
        p[i] = 0;
    l->sr = sr;
    l->n23 = (int)(0.02322f * sr + 0.5f);
    l->n11 = (int)(0.011f * sr + 0.5f);
    l->rng = seed ? seed : 0x9e3779b9u;
    l->line[0].buf = dl1;
    l->line[1].buf = dl2;
    for (int i = 0; i < 2; i++) {
        l->line[i].len = len;
        l->line[i].comp.atk = l->line[i].comp.rel = fexp2(-4.60517f * 1.44269504f / (2.5f * sr / 1000.f));
    }
    for (int v = 0; v < 4; v++)
        l->v[v].vib_rate = 0.5f + 3.f * (float)((rnd(l) + 1.f) * 500.f) * 0.001f;
    static const float defs[LIRA_NPARAMS] = {
        0, 78, 74, 36, 27, 57, 60, 51, 1, 1, 1, 1, 14.3774f, 43.1321f, 103.038f, 71.8868f, 0, 0, 0, 0, 0, 0, 0, 0,
        66, 65, 0, 0, 0, 0, 1, 86, 69, 127, 42, 64, 1, 1, 94.5745f, 56.7447f, 54.0426f, 67.5532f, 40.7358f, 47.9245f,
        2, 0, 0};
    for (int i = 0; i < LIRA_NPARAMS; i++)
        lira_param(l, i, defs[i]);
    lira_settle(l);
    for (int v = 0; v < 4; v++)
        l->v[v].osc[0].pwh = l->v[v].osc[1].pwh = 0.53125f;
}

void lira_init(lira_t *l, float sr, float *dl1, float *dl2, int len, uint32_t seed)
{
    for (int k = 0; k < len; k++)
        dl1[k] = dl2[k] = 0.f;
    lira_reset(l, sr, dl1, dl2, len, seed);
}

/* Every ramp to its target now, as after Pd's loadbang + settling. Touch envelopes too: they rest where they are. */
void lira_settle(lira_t *l)
{
    for (int v = 0; v < 4; v++) {
        lira_voice *vo = &l->v[v];
        rjump(&vo->tune[0], vo->tune[0].tgt); rjump(&vo->tune[1], vo->tune[1].tgt);
        rjump(&vo->sharp, vo->sharp.tgt); rjump(&vo->mod, vo->mod.tgt); rjump(&vo->vib, vo->vib.tgt);
        rjump(&vo->env[0], vo->env[0].tgt); rjump(&vo->env[1], vo->env[1].tgt);
    }
    rjump(&l->hold[0], l->hold[0].tgt); rjump(&l->hold[1], l->hold[1].tgt);
    rjump(&l->lfo_a, l->lfo_a.tgt); rjump(&l->lfo_b, l->lfo_b.tgt);
    rjump(&l->fbline, l->fbline.tgt); rjump(&l->dmix, l->dmix.tgt); rjump(&l->drv_g, l->drv_g.tgt);
    rjump(&l->dst, l->dst.tgt); rjump(&l->vol, l->vol.tgt);
    for (int i = 0; i < 2; i++) {
        rjump(&l->line[i].time, l->line[i].time.tgt);
        rjump(&l->line[i].mod, l->line[i].mod.tgt);
    }
}

static float tap(const lira_line *ln, float d)
{
    float rp = (float)ln->w - d;
    while (rp < 0.f)
        rp += (float)ln->len;
    int i = (int)rp;
    float f = rp - (float)i;
    int j = i + 1;
    if (i >= ln->len)
        i -= ln->len;
    if (j >= ln->len)
        j -= ln->len;
    return ln->buf[i] + f * (ln->buf[j] - ln->buf[i]);
}

void lira_process(lira_t *l, float *out, int n)
{
    const float sr = l->sr, isr = 1.f / sr;
    const float hp3c = 1.f - 3.f * TWO_PI * isr, hp1c = 1.f - 1.f * TWO_PI * isr;
    const float hp20c = 1.f - 20.f * TWO_PI * isr, hp10c = 1.f - 10.f * TWO_PI * isr;
    const float lp4k = clip_(4000.f * TWO_PI * isr, 0.f, 1.f), lp689 = clip_(689.f * TWO_PI * isr, 0.f, 1.f);
    static const int busa[4] = {1, 0, 3, 2}, busb[4] = {3, 0, 1, 2};
    const float sw = l->params[LIRA_SWITCH] > 0.5f, tfb = l->params[LIRA_TOTAL_FB] > 0.5f;
    const float gsa = 0.999f * (sw == 0.f) + 0.001f, gsb = 0.999f * sw + 0.001f;
    const float gfb = 0.999f * tfb + 0.001f, gnf = 0.999f * (tfb == 0.f) + 0.001f;
    const float andor = l->params[LIRA_ANDOR];
    const float link = l->params[LIRA_LINK] > 0.5f ? 1.f : 0.f;
    const int delmod = (int)l->params[LIRA_DEL_MOD], lfowav = (int)l->params[LIRA_LFO_WAV];
    const float msr = sr * 0.001f, dmax = (float)(l->line[0].len - 3);
    float g0[4], g2[4];
    for (int v = 0; v < 4; v++) {
        int src = (int)l->params[LIRA_SOURCE12 + v];
        g0[v] = 0.999f * (src == 0) + 0.001f;
        g2[v] = 0.999f * (src == 2) + 0.001f;
    }

    while (n > 0) {
        const int m = n < LIRA_CR ? n : LIRA_CR;

        /* ---- control block: ramps, LFOs, vibrato, envelopes, coefficients ---- */
        rstep(&l->lfo_a, m);
        rstep(&l->lfo_b, m);
        float sqa = clip_(1000.f * fcos(l->pa), -1.f, 1.f), sqb = clip_(1000.f * fcos(l->pb), -1.f, 1.f);
        float sqsum = (sqa + sqb) * 0.5f;
        float sqr_lfo = andor == 0.f ? sqa * sqb : andor * sqsum;
        float tri_lfo = ((fabs_(l->pa - 0.5f) * 4.f - 1.f) + (fabs_(l->pb - 0.5f) * 4.f - 1.f)) * 0.5f;
        float fb = l->lfo_b.cur;
        l->pa = wrap1(l->pa + l->lfo_a.cur * isr * (float)m);
        l->pb = wrap1(l->pb + (fb + link * sqa * fb * 0.5f) * isr * (float)m);
        float lfo_fm = sqr_lfo * gnf;       /* the square-LFO half of the "lfo" source; the feedback half is per sample */

        rstep(&l->hold[0], m);
        rstep(&l->hold[1], m);
        float vmod[4], vsh[4], vpw[4], vf[8], vgain[8], vbump[8];
        for (int v = 0; v < 4; v++) {
            lira_voice *vo = &l->v[v];
            rstep(&vo->mod, m);
            rstep(&vo->vib, m);
            rstep(&vo->sharp, m);
            vo->vib_ph = wrap1(vo->vib_ph + vo->vib_rate * isr * (float)m);
            float vs = fcos(vo->vib_ph - 0.25f) * vo->vib.cur;       /* osc~ is a sine */
            vmod[v] = vo->mod.cur;
            vsh[v] = vo->sharp.cur;
            vpw[v] = 0.53125f + vs * 0.025f;
            float hold = l->hold[v >> 1].cur;
            for (int k = 0; k < 2; k++) {
                rstep(&vo->tune[k], m);
                rstep(&vo->env[k], m);
                float f = vo->tune[k].cur * (1.f + vs * 0.005f), e = vo->env[k].cur;
                vf[2 * v + k] = f;
                vo->osc[k].a = lp_coef(f, sr);
                vgain[2 * v + k] = fmin_(e * e + hold, 1.f);
                vbump[2 * v + k] = vo->env[k].n ? 0.5f * fcos(e - 0.25f) : 0.f;   /* 0 once at rest at 0 or 1 */
            }
        }

        rstep(&l->fbline, m);
        rstep(&l->dmix, m);
        rstep(&l->drv_g, m);
        rstep(&l->dst, m);
        rstep(&l->vol, m);
        const float fbl = l->fbline.cur, wetg = 0.5f / fmax_(fbl, 1.5f), dm = l->dmix.cur;
        const float g = l->drv_g.cur, ig = 1.f / clip_(g, 1.f, 4.f), dstm = l->dst.cur, vol = l->vol.cur;
        const float lfom = delmod == 2 ? (lfowav ? sqsum : tri_lfo) : 0.f;
        float ltime[2], lmod[2];
        for (int i = 0; i < 2; i++) {
            lira_line *ln = &l->line[i];
            rstep(&ln->time, m);
            rstep(&ln->mod, m);
            rstep(&ln->ex.red, m);
            ln->ex.gt = db2lin(ln->ex.red.cur);
            ltime[i] = ln->time.cur;
            lmod[i] = ln->mod.cur;
        }

        /* ---- audio ---- */
        for (int s = 0; s < m; s++) {
            float mix = 0.f, busn[4];
            for (int v = 0; v < 4; v++) {
                lira_voice *vo = &l->v[v];
                float msrc = g0[v] * (l->bus[busa[v]] * gsa + l->bus[busb[v]] * gsb) +
                             g2[v] * (l->total_fb * gfb + lfo_fm);
                float fm = 1.f + msrc * vmod[v], sh = vsh[v], y = 0.f;
                for (int k = 0; k < 2; k++) {
                    int o = 2 * v + k;
                    float tr, sq = osc_tick(&vo->osc[k], vf[o] * fm, vpw[v], isr, &tr);
                    y += (tr + sh * (sq - tr) + vbump[o]) * vgain[o];
                }
                mix += y;
                busn[v] = hip(&vo->hp, y, hp3c);
            }
            for (int v = 0; v < 4; v++)
                l->bus[v] = busn[v];

            /* dual delay */
            float u = mix * 0.166667f + rnd(l) * 0.001f, wet = 0.f;
            for (int i = 0; i < 2; i++) {
                lira_line *ln = &l->line[i];
                float self = lop(&ln->tap_lp, delmod == 0 ? tap(ln, 64.f) * 0.5f : 0.f, lp689);
                float d = clip_((ltime[i] + (self + lfom) * lmod[i]) * msr, 64.f, dmax);
                float y = tap(ln, d);
                y = hip(&ln->hp_x, y, hp1c);
                y = lop(&ln->lp_in, y, lp4k);
                y = comp_run(&ln->comp, y);
                y = exp_run(&ln->ex, y, l->n11);
                wet += y;
                ln->buf[ln->w] = u + y * fbl;
                if (++ln->w >= ln->len)
                    ln->w = 0;
            }
            float dly = u + dm * (ptanh(wet * wetg) - u);

            /* master: drive -> tanh (+ positive spike) -> mix with dry -> tanh -> volume */
            float t = ptanh(hip(&l->hp1, dly * g, hp20c)), pw31 = 0.f;
            if (t > 0.f) {
                float t2 = t * t, t4 = t2 * t2, t8 = t4 * t4, t16 = t8 * t8;
                pw31 = t16 * t8 * t4 * t2 * t;
            }
            float dist = hip(&l->hp2, t + pw31 * 0.25f, hp10c) * ig;
            float yv = ptanh(dly * (1.f - dstm) + (dly * 0.1f + dist) * dstm);
            l->total_fb = hip(&l->hp3, yv, hp3c);
            *out++ = yv * vol;
        }
        n -= m;
    }
}
