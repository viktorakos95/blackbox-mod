/*
 * LIRA-8 engine, see lira.h. The signal flow (8 voices in 4 pairs with cross-FM, hyper-LFO, dual delay, drive) follows
 * Mike Moreno DSP's LIRA-8 plugin (Pure Data); the circuits follow what is known of the SOMA Lyra-8 hardware:
 * docs/LIRA-analog-notes.md lists every source and which constants are assumptions.
 *
 *  - voices: HEF4093 Schmitt relaxation oscillators, simulated directly (timing capacitor between two thresholds, exact
 *    crossing time, PolyBLEP on the square); the triangle is the capacitor voltage. Per-unit tolerances from a seed.
 *  - envelope: RC charge / discharge, part of the oscillator: it bends the waveform and the pitch a little; there is no
 *    output VCA, so closed voices leak and neighbours couple through the shared supply.
 *  - delay: two PT2399 (44,000-entry memory, VCO clock, 1-bit codec at long times) with one shared feedback loop; the
 *    modulation moves the clock, not a tap.
 *
 * For the Cortex-M7's budget everything slow runs once per LIRA_CR-sample block (ramps, LFOs, envelopes, coefficients);
 * oscillators, FM, the delay and the output stage run per sample. No libm: small polynomial exp2/log2/cos.
 */
#include "lira.h"

#pragma GCC optimize("O2", "no-tree-loop-distribute-patterns")   /* no libc to call */

#define TWO_PI 6.2831853f
#define LIRA_CR 16          /* control block, samples */

/* ---- model constants; the ones marked (assumed) are judgement calls, see the notes ---- */
#define VP_TYP        0.562f     /* HEF4093B on the Lyra-8's 5.87 V rail (DIY docs, "Tweaks"): 3.3 V / 5.87 V */
#define VN_TYP        0.434f     /* 2.55 V / 5.87 V */
#define TOL_THR       0.035f     /* (assumed) threshold spread, relative, per voice */
#define TRI_AMP       0.80f
#define SQ_AMP        0.59f
#define LEAK_BASE     0.0005f    /* (assumed) bleed of a closed voice at 1 kHz, ~ -66 dB; rises with sqrt(f) */
#define COUPLE        0.003f     /* (assumed) shared-supply coupling of neighbouring oscillators, in rail units */
#define ENV_PITCH     0.003f     /* (assumed) the envelope pulls pitch: closed voices a little sharp */
#define WSHAPE        0.25f      /* (assumed) transistor curve: bend grows as the envelope closes */
#define REL_SLOW      3.3f       /* s, RC time constant (the hardware's 3 Mohm x 2.2 uF is 6.6 s; halved on request: it was far too long
                                    to play with); the gain is the envelope squared, so -60 dB takes ~3.5 tau = 11.5 s; +-20 % per voice */
#define REL_FAST      0.145f     /* s, FAST: -60 dB in 0.5 s */
#define ATTACK_TAU    0.2f       /* (assumed) s at full touch: attack resistor 300 k-1 M with 2.2 uF (DIY docs) */
#define HOLD_SLOW     0.5f       /* knob position where slow-envelope voices start to drone (12 o'clock) */
#define HOLD_FAST     0.7f       /* ... FAST voices (2 o'clock) */
#define PT_BITS       44000.0f   /* PT2399 memory */
#define TD_MIN        0.031f     /* s, PT2399 at 22 MHz */
#define MOD_MAX       0.45f      /* (assumed) maximum clock modulation, relative */
#define ADM_MIN       0.0006f    /* (assumed) adaptive delta codec step limits */
#define ADM_MAX       0.25f

#ifdef __ARM_FP                  /* VABS, VMINNM, VMAXNM: no compare, no branch to mispredict (same results for numbers) */
static inline float fabs_(float x) { return __builtin_fabsf(x); }
static inline float fmin_(float a, float b) { return __builtin_fminf(a, b); }
static inline float fmax_(float a, float b) { return __builtin_fmaxf(a, b); }
#else
static inline float fabs_(float x) { return x < 0.f ? -x : x; }
static inline float fmin_(float a, float b) { return a < b ? a : b; }
static inline float fmax_(float a, float b) { return a > b ? a : b; }
#endif
static inline float clip_(float x, float lo, float hi) { return fmin_(fmax_(x, lo), hi); }
static inline float floor_(float x) { int i = (int)x; return (float)(i - ((float)i > x)); }

static float fexp2(float x)    /* 2^x, relative error < 3e-9 (envelopes apply it every 0.4 ms: it must be accurate near 0) */
{
    if (x < -126.f)
        return 0.f;
    if (x > 126.f)
        x = 126.f;
    float r = floor_(x + 0.5f), f = x - r;
    float p = 1.f + f * (0.693147207f + f * (0.240226512f + f * (0.055503272f + f * (0.009618026f +
              f * (0.001340043f + f * 0.000154697f)))));
    union { float f; uint32_t u; } v;
    v.u = (uint32_t)((int)r + 127) << 23;
    return p * v.f;
}

static float flog2(float x)    /* log2, |error| < 2e-5 */
{
    union { float f; uint32_t u; } v;
    v.f = x;
    float e = (float)((int)((v.u >> 23) & 255) - 127);
    v.u = (v.u & 0x7fffffu) | 0x3f800000u;
    float t = v.f - 1.f;
    return e + t * (1.44159208f + t * (-0.70725343f + t * (0.41156148f + t * (-0.18983245f + t * 0.04392863f))));
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
static inline float expm_small(float x)     /* exp(-x), x < 0.2, error < 7e-5 (envelope steps) */
{
    return 1.f - x * (1.f - x * 0.5f * (1.f - x * (1.f / 3.f)));
}

/* Pade tanh, as ma.tanh~ */
static float ptanh(float x)
{
    x = clip_(x, -3.f, 3.f);
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

static float wrap1(float x) { return x - floor_(x); }

/* ---- line~ (knob smoothing) ---- */
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
static float urand(lira_t *l, float lo, float hi) { return lo + (hi - lo) * 0.5f * (rnd(l) + 1.f); }

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

/* ---- a tone generator ---- */
static void gen_setup(lira_t *l, lira_gen *g)
{
    g->vp = VP_TYP * (1.f + TOL_THR * rnd(l));
    g->vn = VN_TYP * (1.f + TOL_THR * rnd(l));
    if (g->vp < g->vn + 0.08f)
        g->vp = g->vn + 0.08f;
    /* half periods of the RC loop: charge Vn -> Vp towards the rail, discharge Vp -> Vn towards ground */
    g->kt = 0.69314718f * (flog2((1.f - g->vn) / (1.f - g->vp)) + flog2(g->vp / g->vn));
    g->lr = 1.f / flog2((1.f - g->vn) / (1.f - g->vp));
    g->lf = 1.f / flog2(g->vp / g->vn);
    g->lrt = g->lr * 1.44269504f;
    g->lft = g->lf * 1.44269504f;
    g->linp = 0.f;
    g->mid = 0.5f * (g->vp + g->vn);
    g->inv_half = 2.f / (g->vp - g->vn);
    g->v = urand(l, g->vn, g->vp);
    g->s = rnd(l) > 0.f;
    g->t_slow = REL_SLOW * urand(l, 0.8f, 1.2f);
    g->t_fast = REL_FAST * urand(l, 0.8f, 1.25f);
    g->trim = db2lin(1.5f * rnd(l));                      /* SoS: some variation between voice levels */
    g->leak = LEAK_BASE * urand(l, 0.6f, 1.6f);
    g->vib_depth = urand(l, 0.0035f, 0.0065f);
    g->vib_ph = urand(l, 0.f, 1.f);
    g->u = clip_(g->s ? flog2((1.f - g->vn) / fmax_(1.f - g->v, 1e-4f)) * g->lr : flog2(g->vp / fmax_(g->v, 1e-4f)) * g->lf, 0.f, 1.f);
}

/* Frequency modulation follows the plugin: f * (1 + mod), the oscillator stalls when that goes below zero. (The hardware
   notes suggest an additive deviation in Hz instead; the plugin's law is kept because it is the reference sound.) */

/* Vibrato rates of the eight voices: 30..56 k resistors with 10 uF (DIY BOM), 3.5-6.5 Hz; which voice has which varies per unit. */
static const float VIB_HZ[8] = {6.47f, 5.89f, 5.40f, 4.98f, 4.62f, 4.52f, 3.81f, 3.47f};

/* One sample of the oscillator. f = frequency, cpl = supply coupling from the neighbours (rail units). Returns the
   square and triangle one sample late (PolyBLEP corrects the sample before and after each edge). */
static inline __attribute__((always_inline)) void gen_tick(lira_gen *g, float x, float cpl, int lin, float *tri, float *sq)
{
    x = clip_(x, 0.f, 0.5f);                                       /* x: f * kt / sr, the time step in units of RC */
    float a = x * (1.f - x * 0.5f * (1.f - x * (1.f / 3.f)));     /* 1 - exp(-x) */
    float v0 = g->v, v = v0 + ((g->s ? 1.f : 0.f) + cpl - v0) * a;
    float emit_sq = g->sqp, emit_tri = lin ? g->linp : g->trip, d = 0.f, h = 0.f, thr = 0.f;
    if (g->s) {
        if (v >= g->vp) {
            d = v > v0 ? clip_(1.f - (g->vp - v0) / (v - v0), 0.f, 1.f) : 1.f;   /* already past it: switch now */
            g->s = 0;
            h = -2.f;
            thr = g->vp;
        }
    } else if (v <= g->vn) {
        d = v < v0 ? clip_(1.f - (g->vn - v0) / (v - v0), 0.f, 1.f) : 1.f;
        g->s = 1;
        h = 2.f;
        thr = g->vn;
    }
    if (h != 0.f) {          /* the rest of this sample, d of it, is spent charging towards the new target */
        float xd = x * d, ad = xd * (1.f - xd * 0.5f * (1.f - xd * (1.f / 3.f)));
        v = thr + ((g->s ? 1.f : 0.f) + cpl - thr) * ad;
    }
    g->v = v;
    float cur = g->s ? 1.f : -1.f;
    if (h != 0.f) {
        emit_sq += 0.5f * h * d * d;
        cur -= 0.5f * h * (1.f - d) * (1.f - d);
    }
    g->sqp = cur;
    if (lin) {  /* the triangle linear in time (for the sine): the capacitor's time t = ln((1 - vn) / (1 - v)) charging, ln(vp / v) discharging, runs by x a sample
                   whatever the frequency does (FM included), so the fraction of the half period just goes up by x / (the half's ln ratio): no logarithm, no division */
        float u = h != 0.f ? d * x * (g->s ? g->lrt : g->lft) : g->u + x * (g->s ? g->lrt : g->lft);
        u = fmin_(u, 1.f);
        g->u = u;
        g->linp = g->s ? 2.f * u - 1.f : 1.f - 2.f * u;
    } else      /* (only the waveform in use is kept: gen_sync brings the other one back when WAVE changes) */
        g->trip = (v - g->mid) * g->inv_half;
    *sq = emit_sq;
    *tri = emit_tri;
}

/* WAVE changed: the triangle now in use, from the capacitor (lin: how far through the half period, as gen_setup) */
static void gen_sync(lira_gen *g, int lin)
{
    if (lin) {
        g->u = clip_(g->s ? flog2((1.f - g->vn) / fmax_(1.f - g->v, 1e-4f)) * g->lr : flog2(g->vp / fmax_(g->v, 1e-4f)) * g->lf, 0.f, 1.f);
        g->linp = g->s ? 2.f * g->u - 1.f : 1.f - 2.f * g->u;
    } else
        g->trip = (g->v - g->mid) * g->inv_half;
}

/* the SHARP clipper's curve at z (|z| <= 1), times 16 */
static inline float squash(float z)
{
    float z2 = z * z;
    return z * (35.f + z2 * (-35.f + z2 * (21.f - 5.f * z2)));
}

/* sin(pi/2 x) for -1 <= x <= 1 from the oscillator's triangle: an odd polynomial, error 1.6e-4 (-76 dB), so the sine's own
   harmonics stay below the delay's noise floor */
static inline float sinsh(float x)
{
    x = clip_(x, -1.f, 1.f);
    float x2 = x * x;
    return x * (1.57043715f + x2 * (-0.64270926f + x2 * 0.0724327f));
}

/* ---- helpers ---- */
static const float TUNE_LO[8] = {-16.f, -16.f, 7.f, 9.f, 20.f, 20.f, 33.f, 33.f};
static const float TUNE_HI[8] = {93.f, 93.f, 109.f, 107.f, 116.54f, 116.54f, 126.24f, 131.22f};

float lira_tune_for_midi(int o, float midi)
{
    float p = (midi - TUNE_LO[o & 7]) / (TUNE_HI[o & 7] - TUNE_LO[o & 7]);
    return (p < 0.f ? 0.f : p > 1.f ? 1.f : p) * 127.f;
}

void lira_tune_range(int o, float *lo, float *hi)
{
    *lo = TUNE_LO[o & 7];
    *hi = TUNE_HI[o & 7];
}

/* Tempo sync: LFO `which` (0 A, 1 B) at hz, its phase locked to `phase` (0..1) unless negative */
void lira_lfo_sync(lira_t *l, int which, float hz, float phase)
{
    rset(which ? &l->lfo_b : &l->lfo_a, hz, l->n23);
    if (phase >= 0.f)
        *(which ? &l->pb : &l->pa) = wrap1(phase);
}

/* ... and delay line `which` at `sec` (folded by octaves into what the PT2399 can do) */
void lira_delay_sync(lira_t *l, int which, float sec)
{
    const float lo = TD_MIN, hi = TD_MIN * fexp2(7.5826f);
    if (!(sec > 1e-4f))
        return;
    while (sec > hi)
        sec *= 0.5f;
    while (sec < lo)
        sec *= 2.f;
    rset(&l->line[which & 1].time, sec, l->n23);
}

static void retune(lira_t *l, int o)
{
    const float *lo = TUNE_LO, *hi = TUNE_HI;
    float p = l->params[LIRA_TUNE1 + o] * (1.f / 127.f);
    float pitch = l->params[o < 4 ? LIRA_PITCH1234 : LIRA_PITCH5678] * (1.f / 127.f) * 1.99f + 0.01f;
    float f = mtof(lo[o] + p * (hi[o] - lo[o])) * pitch;
    if (l->params[LIRA_QUANTIZE] > 0.5f)
        f = mtof(floor_(ftom(f) + 0.5f));
    rset(&l->v[o >> 1].g[o & 1].tune, f, l->n23);
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
        l->v[id - LIRA_FAST12].fast = x > 0.5f;
        l->v[id - LIRA_FAST12].follow = x > 1.5f;
        break;
    case LIRA_SHARP12: case LIRA_SHARP34: case LIRA_SHARP56: case LIRA_SHARP78:
        rset(&l->v[id - LIRA_SHARP12].sharp, d * d, n);
        break;
    case LIRA_MOD12: case LIRA_MOD34: case LIRA_MOD56: case LIRA_MOD78:
        rset(&l->v[id - LIRA_MOD12].mod, 2.f * d * d, n);   /* the plugin has d^4; d^2 is audible from mid-knob */
        break;
    case LIRA_HOLD1234: case LIRA_HOLD5678:
        rset(&l->hold[id - LIRA_HOLD1234], d, n);
        break;
    case LIRA_VIBRATO:
        rset(&l->vibsw, x >= 0.5f ? 1.f : 0.f, n);
        break;
    case LIRA_DRV: rset(&l->drv_g, db2lin(3.f + fpow(3.f, 2.f * d + 1.f)), n); break;
    case LIRA_DST_MIX: rset(&l->dst, d, n); break;
    case LIRA_VOL: rset(&l->vol, d * d, n); break;
    case LIRA_F_A: rset(&l->lfo_a, mtof(fpow(d, 2.f) * 127.f - 75.f), n); break;
    case LIRA_F_B: rset(&l->lfo_b, mtof(fpow(d, 2.f) * 127.f - 75.f), n); break;
    case LIRA_TIME1: case LIRA_TIME2:   /* the pot sets the VCO current: exponential from the chip's 31 ms floor */
        rset(&l->line[id - LIRA_TIME1].time, TD_MIN * fexp2(d * 7.5826f), n);
        break;
    case LIRA_FEEDBACK: rset(&l->fbline, fexp2(2.f * d) * d, n); break;
    case LIRA_DEL_MIX: rset(&l->dmix, d, n); break;
    case LIRA_EXT: l->ext_g = 2.f * d * d; break;
    case LIRA_LVL1: case LIRA_LVL2: case LIRA_LVL3: case LIRA_LVL4:
    case LIRA_LVL5: case LIRA_LVL6: case LIRA_LVL7: case LIRA_LVL8:
        rset(&l->v[(id - LIRA_LVL1) >> 1].g[(id - LIRA_LVL1) & 1].lvl, d * d, n);
        break;
    case LIRA_MOD1: case LIRA_MOD2:
        rset(&l->line[id - LIRA_MOD1].mod, MOD_MAX * d * d, n);
        break;
    default:
        break;
    }
}

void lira_touch(lira_t *l, int s, float drive)
{
    l->v[s >> 1].g[s & 1].drive = clip_(drive, 0.f, 1.f);
}

void lira_key(lira_t *l, int s, int on) { lira_touch(l, s, on ? 1.f : 0.f); }

int lira_size(void) { return (int)sizeof(lira_t); }

void lira_tone(lira_t *l, float width, float drift, float harm)
{
    l->harm = clip_(harm, 0.f, 1.f);
    l->width = clip_(width, 0.f, 1.f);
    l->drift = clip_(drift, 0.f, 1.f);
}

void lira_reset(lira_t *l, float sr, int16_t *dl1, int16_t *dl2, int len, uint32_t seed)
{
    char *p = (char *)l;
    for (unsigned i = 0; i < sizeof *l; i++)
        p[i] = 0;
    l->sr = sr;
    l->bodyc = 0.41f;                                     /* (the bass lift of the original; width and drift are off until a page asks) */
    l->harm = LIRA_HARM;
    l->n23 = (int)(0.02322f * sr + 0.5f);
    l->rng = seed ? seed : 0x9e3779b9u;
    l->line[0].buf = dl1;
    l->line[1].buf = dl2;
    for (int i = 0; i < 2; i++) {
        l->line[i].len = len;
        l->line[i].stp = ADM_MIN;
        l->line[i].pbit = 1;
    }
    int perm[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    for (int i = 7; i > 0; i--) {
        int j = (int)(0.5f * (rnd(l) + 1.f) * (float)(i + 1)), t = perm[i];
        if (j > i)
            j = i;
        perm[i] = perm[j];
        perm[j] = t;
    }
    for (int v = 0; v < 4; v++)
        for (int k = 0; k < 2; k++) {
            gen_setup(l, &l->v[v].g[k]);
            l->v[v].g[k].vib_rate = VIB_HZ[perm[2 * v + k]] * urand(l, 0.92f, 1.08f);
        }
    {   /* where each oscillator sits in the stereo image: eight evenly spaced places from left to right, dealt out at random (so no side gets crowded), each nudged a little */
        int spot[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        for (int i = 7; i > 0; i--) {
            int j = (int)(0.5f * (rnd(l) + 1.f) * (float)(i + 1)), t = spot[i];
            if (j > i)
                j = i;
            spot[i] = spot[j];
            spot[j] = t;
        }
        for (int o = 0; o < 8; o++)
            l->pan[o] = clip_(((float)spot[o] - 3.5f) * (0.85f / 3.5f) + 0.08f * rnd(l), -1.f, 1.f);
    }
    static const float defs[LIRA_NPARAMS] = {
        0, 78, 74, 36, 27, 57, 60, 51, 1, 1, 1, 1, 14.3774f, 43.1321f, 103.038f, 71.8868f, 0, 0, 0, 0, 0, 0, 0, 0,
        66, 65, 0, 0, 0, 0, 1, 86, 69, 127, 42, 64, 1, 1, 94.5745f, 56.7447f, 54.0426f, 67.5532f, 40.7358f, 47.9245f,
        2, 0, 0, 0, 0, 127, 127, 127, 127, 127, 127, 127, 127};
    for (int i = 0; i < LIRA_NPARAMS; i++)
        lira_param(l, i, defs[i]);
    lira_settle(l);
}

void lira_init(lira_t *l, float sr, int16_t *dl1, int16_t *dl2, int len, uint32_t seed)
{
    for (int k = 0; k < len; k++)
        dl1[k] = dl2[k] = 0;
    lira_reset(l, sr, dl1, dl2, len, seed);
}

/* Every ramp to its target now, as after Pd's loadbang + settling. Envelopes rest where they are. */
void lira_settle(lira_t *l)
{
    for (int v = 0; v < 4; v++) {
        lira_voice *vo = &l->v[v];
        for (int k = 0; k < 2; k++) {
            lira_gen *g = &vo->g[k];
            rjump(&g->tune, g->tune.tgt);
            rjump(&g->lvl, g->lvl.tgt);
            float f0 = g->tune.cur;
            g->leak_f = g->leak * fexp2(0.5f * flog2(fmax_(f0, 1.f) * 0.001f));
        }
        rjump(&vo->sharp, vo->sharp.tgt);
        rjump(&vo->mod, vo->mod.tgt);
    }
    rjump(&l->hold[0], l->hold[0].tgt); rjump(&l->hold[1], l->hold[1].tgt);
    rjump(&l->vibsw, l->vibsw.tgt);
    rjump(&l->lfo_a, l->lfo_a.tgt); rjump(&l->lfo_b, l->lfo_b.tgt);
    rjump(&l->fbline, l->fbline.tgt); rjump(&l->dmix, l->dmix.tgt); rjump(&l->drv_g, l->drv_g.tgt);
    rjump(&l->dst, l->dst.tgt); rjump(&l->vol, l->vol.tgt);
    for (int i = 0; i < 2; i++) {
        rjump(&l->line[i].time, l->line[i].time.tgt);
        rjump(&l->line[i].mod, l->line[i].mod.tgt);
    }
}

/* ---- one PT2399 ---- */
#define DL_SCALE 12000.f                                  /* 16-bit memory: +-2.7 in signal units, 83 microunits a step (-78 dB) */
static inline void dl_put(lira_line *ln, int i, float v)
{
    v = clip_(v, -2.7f, 2.7f) * DL_SCALE;
    ln->buf[i] = (int16_t)(v + (v >= 0.f ? 0.5f : -0.5f));
}
static inline float dl_get(const lira_line *ln, int i) { return (float)ln->buf[i] * (1.f / DL_SCALE); }

static inline __attribute__((always_inline)) float line_run(lira_line *ln, float in, float mv, float k_ratio, float d0, float depth, const float *c)
{
    /* c: [0] 1 Hz high-pass, [1] input low-pass (5 kHz, two poles), [2] output low-pass (2.8 kHz, one pole: SoS "a single
       capacitor"), [3] 689 Hz (self-modulation sense) */
    float x = hip(&ln->hp_x, in, c[0]);
    x = lop(&ln->in1, x, c[1]);
    x = lop(&ln->in2, x, c[1]);
    float den = fmax_(1.f + depth * mv, 0.25f);          /* the clock runs faster with positive modulation */
    float ratio = k_ratio * den;                          /* memory steps per audio sample */
    float raw;
    if (ratio >= 1.f) {                                   /* clock above the audio rate: a tap */
        dl_put(ln, ln->w, x);
        if (++ln->w >= ln->len)
            ln->w = 0;
        float dly = clip_(d0 / den, 4.f, PT_BITS), rp = (float)ln->w - dly;
        if (rp < 0.f)
            rp += (float)ln->len;
        int i = (int)rp, j = i + 1;
        float f = rp - (float)i;
        if (i >= ln->len)
            i -= ln->len;
        if (j >= ln->len)
            j -= ln->len;
        float b0 = dl_get(ln, i);
        raw = b0 + f * (dl_get(ln, j) - b0);
    } else {                                              /* clocked below the audio rate: steps, 1-bit adaptive codec */
        ln->acc += ratio;
        if (ln->acc >= 1.f) {
            ln->acc -= 1.f;
            float xi = clip_(x, -1.5f, 1.5f);
            int bit = xi > ln->est ? 1 : -1;
            ln->stp = clip_(ln->stp * (bit == ln->pbit ? 1.5f : 0.66f), ADM_MIN, ADM_MAX);
            ln->pbit = bit;
            ln->est = clip_(ln->est + (float)bit * ln->stp, -2.f, 2.f);
            dl_put(ln, ln->w, ln->est);
            if (++ln->w >= ln->len)
                ln->w = 0;
        }
        int r = ln->w - (int)PT_BITS;
        if (r < 0)
            r += ln->len;
        raw = dl_get(ln, r);                              /* zero-order hold: the converter's staircase */
    }
    /* the output filter follows the clock: a slowed PT2399 is dark, which is what keeps its steps from sounding as hiss */
    float oc = ratio >= 1.f ? c[2] : c[2] * fmax_(ratio, 0.3f);
    float y = lop(&ln->out1, raw, oc);
    if (ratio < 1.f)
        y = lop(&ln->out2, y, oc);
    lop(&ln->self_lp, y, c[3]);
    return y;
}

/* (always inlined into the two entry points below, so the mono one carries none of the right channel's work) */
static inline __attribute__((always_inline)) void process_(lira_t *l, float *out, int n, const int stereo)
{
    const float sr = l->sr, isr = 1.f / sr;
    const float hp3c = 1.f - 3.f * TWO_PI * isr, hp1c = 1.f - 1.f * TWO_PI * isr;
    const float hp20c = 1.f - 20.f * TWO_PI * isr, hp10c = 1.f - 10.f * TWO_PI * isr;
    const float dc[4] = {hp1c, clip_(5000.f * TWO_PI * isr, 0.f, 1.f), clip_(2800.f * TWO_PI * isr, 0.f, 1.f),
                         clip_(689.f * TWO_PI * isr, 0.f, 1.f)};
    const float lp3k = clip_(3000.f * TWO_PI * isr, 0.f, 1.f), lp150 = clip_(150.f * TWO_PI * isr, 0.f, 1.f);
    const float lp9k = clip_(9000.f * TWO_PI * isr, 0.f, 1.f);
    static const int busa[4] = {1, 0, 3, 2}, busb[4] = {3, 0, 1, 2};
    const float sw = l->params[LIRA_SWITCH] > 0.5f, tfb = l->params[LIRA_TOTAL_FB] > 0.5f;
    const float gsa = 0.999f * (sw == 0.f) + 0.001f, gsb = 0.999f * sw + 0.001f;
    const float gfb = 0.999f * tfb + 0.001f, gnf = 0.999f * (tfb == 0.f) + 0.001f;
    const float andor = l->params[LIRA_ANDOR];
    const float link = l->params[LIRA_LINK] > 0.5f ? 1.f : 0.f;
    const int delmod = (int)l->params[LIRA_DEL_MOD], lfowav = (int)l->params[LIRA_LFO_WAV];
    float g0[4], g2[4], g3[4];
    int sidx[4];                                     /* FM Source P1..P4: the pair whose output modulates this one */
    const float ka = clip_(1.f / (0.003f * sr), 0.f, 1.f), kr = 1.f / (0.12f * sr);
    for (int v = 0; v < 4; v++) {
        int src = (int)l->params[LIRA_SOURCE12 + v];
        g0[v] = 0.999f * (src == 0) + 0.001f;
        g2[v] = 0.999f * (src == 2) + 0.001f;
        g3[v] = (src == 3) ? 1.f : 0.f;
        sidx[v] = src >= 4 && src <= 7 ? src - 4 : -1;
    }

    const int sine = l->params[LIRA_WAVE] > 0.5f;
    if (sine != l->wave_on) {
        for (int o = 0; o < 8; o++)
            gen_sync(&l->v[o >> 1].g[o & 1], sine);
        l->wave_on = sine;
    }
    /* what every sample reads and writes, in locals for the block (the compiler cannot keep struct members in registers across the delay's stores) */
    float bus[4], sqv[8], tfb_s = l->total_fb, fol = l->fol;
    for (int v = 0; v < 4; v++)
        bus[v] = l->bus[v];
    for (int o = 0; o < 8; o++)
        sqv[o] = l->sq[o];
    const int nlines = l->shed >= 2 ? 0 : l->shed == 1 ? 1 : 2;
    float *out_r = stereo ? l->out_r : 0;
    const float wdt = stereo ? l->width : 0.f;
    /* each oscillator's place in the stereo field: its own random place (l->pan) scaled by the knob (constant power; both 1 at the centre) */
    float pgl[8], pgr[8];
    for (int o = 0; o < 8; o++) {
        float pan = wdt * l->pan[o];
        pgl[o] = wdt > 0.f ? 1.41421356f * fcos((pan + 1.f) * 0.125f) : 1.f;      /* (fcos is an approximation: unity must stay exactly unity) */
        pgr[o] = wdt > 0.f ? 1.41421356f * fcos((1.f - pan) * 0.125f) : 1.f;
    }
    /* drift: each oscillator's pitch is low-passed noise (a few seconds a turn, up to ~6 cents at full) */
    const float drifta = 1.f - (float)LIRA_CR * isr / 6.f;
    const float driftb = l->drift > 0.f ? l->drift * l->drift * 0.0035f * 1.732f * fexp2(0.5f * flog2(2.f * (float)LIRA_CR * isr / 6.f)) : 0.f;
    const float bodyc = l->bodyc, harm = l->harm * 1.2f;
    const float lp160 = clip_(160.f * TWO_PI * isr, 0.f, 1.f), lp350 = clip_(350.f * TWO_PI * isr, 0.f, 1.f);
    int ei = 0;
    const int have_ext = l->ext_l && l->ext_r && l->ext_n > 0;
    float ext = 0.f;
    while (n > 0) {
        const int m = n < LIRA_CR ? n : LIRA_CR;
        const float dt = (float)m * isr;
        l->cb++;

        /* ---- control block ---- */
        rstep(&l->lfo_a, m);
        rstep(&l->lfo_b, m);
        float sqa = clip_(1000.f * fcos(l->pa), -1.f, 1.f), sqb = clip_(1000.f * fcos(l->pb), -1.f, 1.f);
        l->sqa_s += 0.35f * (sqa - l->sqa_s);               /* the analog LFO edges are soft */
        l->sqb_s += 0.35f * (sqb - l->sqb_s);
        float sqsum = (l->sqa_s + l->sqb_s) * 0.5f;
        float sqr_lfo = andor == 0.f ? l->sqa_s * l->sqb_s : andor * sqsum;
        float tri_lfo = ((fabs_(l->pa - 0.5f) * 4.f - 1.f) + (fabs_(l->pb - 0.5f) * 4.f - 1.f)) * 0.5f;
        float fb = l->lfo_b.cur;
        l->pa = wrap1(l->pa + l->lfo_a.cur * dt);
        l->pb = wrap1(l->pb + (fb + link * sqa * fb * 0.5f) * dt);
        float lfo_fm = sqr_lfo * gnf;                        /* the square-LFO half of the "lfo" source */

        rstep(&l->hold[0], m);
        rstep(&l->hold[1], m);
        rstep(&l->vibsw, m);
        const float fx = fol * 14.f;                       /* the external input's level, for Touch = Follow: compressed so quiet
                                                         hits still open the envelope (gain is ve squared, so it must reach 1) */
        const float fd = clip_(1.25f * fx / (fx + 1.f), 0.f, 1.f);
        float vmod[4], vat[4], vas[4], vk[4], vkn[4], vf[8], vx[8], vga[8], vgb[8], vbl[8], vlvl[8], vll[8], vlr[8];
        uint8_t vact[8];
        for (int v = 0; v < 4; v++) {
            lira_voice *vo = &l->v[v];
            rstep(&vo->mod, m);
            rstep(&vo->sharp, m);
            vmod[v] = vo->mod.cur;
            /* SHARP: the triangle (or sine) is squared off gradually by a soft clipper whose drive grows with the knob (no second waveform
               added, so no brittle edge at low settings); over the top sixth the true square is crossfaded in */
            const float sh = vo->sharp.cur;
            if (sh < 0.0005f) {                               /* (the usual case: nothing to compute) */
                vk[v] = vkn[v] = 0.f;
                vat[v] = TRI_AMP;
                vas[v] = 0.f;
            } else {
                const float sc = sh < 0.85f ? sh : 0.85f;     /* (the knob is already squared: this makes the drive follow it with a power of 1.5) */
                const float ks = 30.f * sc * fexp2(0.5f * flog2(sc));
                const float xm = clip_((sh - 0.85f) * (1.f / 0.15f), 0.f, 1.f);
                const float kq = 0.45f * ks;                  /* the clipper's drive: matched to a tanh of drive ks in the harmonics it makes */
                vk[v] = kq > 0.01f ? kq : 0.f;
                vkn[v] = kq > 0.01f ? 1.f / squash(fmin_(kq, 1.f)) : 0.f;
                vat[v] = TRI_AMP * (1.f - xm) * (1.f - 0.32f * (1.f - fexp2(-0.55f * ks)));   /* (squared off, it gets louder: kept near the triangle's level) */
                vas[v] = SQ_AMP * xm;
            }
            float h = l->hold[v >> 1].cur, h0 = vo->fast ? HOLD_FAST : HOLD_SLOW;
            float gh = clip_((h - h0) / (1.f - h0), 0.f, 1.f);
            gh *= gh;
            for (int k = 0; k < 2; k++) {
                lira_gen *gn = &vo->g[k];
                int o = 2 * v + k;
                rstep(&gn->tune, m);
                rstep(&gn->lvl, m);
                gn->vib_ph = wrap1(gn->vib_ph + gn->vib_rate * dt);
                float vib = fcos(gn->vib_ph - 0.25f) * gn->vib_depth * l->vibsw.cur;
                /* envelope: RC. FAST makes the sensors less sensitive; a light touch opens it partly and slowly */
                float dr = vo->fast ? clip_((gn->drive - 0.15f) * (1.f / 0.85f), 0.f, 1.f) : gn->drive;
                if (vo->follow)
                    gn->ve += (fd - gn->ve) * (1.f - expm_small(dt / (fd > gn->ve ? 0.004f : 0.2f)));
                else if (dr > gn->ve)
                    gn->ve += (dr - gn->ve) * (1.f - expm_small(dt * (0.3f + 0.7f * dr) / (ATTACK_TAU * (vo->fast ? 1.4f : 2.f))));
                else if (gn->drive == 0.f)
                    gn->ve *= expm_small(dt / (vo->fast ? gn->t_fast : gn->t_slow));   /* gain = ve^2 */
                float f0 = gn->tune.cur;
                float wander = 0.f;
                if (driftb > 0.f) {
                    l->dr[o] = l->dr[o] * drifta + rnd(l) * driftb;
                    wander += l->dr[o];
                }
                vf[o] = f0 * (1.f + vib + ENV_PITCH * (1.f - gn->ve) + wander);
                vx[o] = vf[o] * gn->kt * isr;            /* the oscillator's time step, before FM */
                const float ga = fmin_(gn->ve * gn->ve + gh, 1.f);
                vact[o] = ga > 2e-4f || gn->drive > 0.f;   /* a closed oscillator (-74 dB and no touch) is not run: its bleed is far below the delay's own noise, and a voice at rest costs nothing */
                if (((l->cb + o) & 7) == 0) {            /* slow function of pitch: one oscillator per block, every 8th */
                    gn->leak_f = gn->leak * fexp2(0.5f * flog2(fmax_(f0, 1.f) * 0.001f));
                }
                /* the envelope's transistor: gain, plus a bend that grows as it closes (waveshaper), the bleed, the trim:
                   (wave * ga + WSHAPE (1 - ga) ga wave^2 + wave * leak) * trim = wave * (vga + vgb * wave) */
                vga[o] = (ga + gn->leak_f) * gn->trim;
                vgb[o] = WSHAPE * (1.f - ga) * ga * gn->trim;
                vbl[o] = vat[v] * gn->leak_f * gn->trim;     /* a closed oscillator: its bleed only */
                vlvl[o] = gn->lvl.cur;
                vll[o] = vlvl[o] * pgl[o];
                vlr[o] = vlvl[o] * pgr[o];
            }
        }

        /* FM: each pair's modulation as weights on the four voice buses, the master output and the inputs (the Pd patch's 0.001
           cross-fade floors kept), depth folded in */
        float wb[4][4], wtf[4], wtc[4], wx[4];
        for (int v = 0; v < 4; v++) {
            for (int j = 0; j < 4; j++)
                wb[v][j] = 0.f;
            wb[v][busa[v]] += g0[v] * gsa;
            wb[v][busb[v]] += g0[v] * gsb;
            if (sidx[v] >= 0)
                wb[v][sidx[v]] += 1.f;
            for (int j = 0; j < 4; j++)
                wb[v][j] *= vmod[v];
            wtf[v] = g2[v] * gfb * vmod[v];
            wtc[v] = g2[v] * lfo_fm * vmod[v];
            wx[v] = g3[v] * vmod[v];
        }

        rstep(&l->fbline, m);
        rstep(&l->dmix, m);
        rstep(&l->drv_g, m);
        rstep(&l->dst, m);
        rstep(&l->vol, m);
        const float fbl = l->fbline.cur, dm = l->dmix.cur;
        const float g = l->drv_g.cur, ig = 1.f / clip_(g, 1.f, 4.f), dstm = l->dst.cur, vol = l->vol.cur;
        const float lfom = delmod == 2 ? (lfowav ? sqsum : tri_lfo) : 0.f;
        float k_ratio[2], d0[2], depth[2];
        for (int i = 0; i < 2; i++) {
            lira_line *ln = &l->line[i];
            rstep(&ln->time, m);
            rstep(&ln->mod, m);
            k_ratio[i] = PT_BITS * isr / ln->time.cur;
            d0[i] = ln->time.cur * sr;
            depth[i] = ln->mod.cur;
        }

        /* ---- audio ---- */
        for (int s = 0; s < m; s++) {
            float mixl = 0.f, mixr = 0.f, busn[4];
            ext = have_ext && ei < l->ext_n ? 0.5f * (l->ext_l[ei] + l->ext_r[ei]) : 0.f;
            fol += (fabs_(ext) - fol) * (fabs_(ext) > fol ? ka : kr);
#pragma GCC unroll 4
            for (int v = 0; v < 4; v++) {
                lira_voice *vo = &l->v[v];
                if (!(vact[2 * v] | vact[2 * v + 1])) {          /* both oscillators closed: only their bleed is left */
                    float yb = 0.f, yl = 0.f, yr = 0.f;
                    for (int k = 0; k < 2; k++) {
                        int o = 2 * v + k;
                        lira_gen *gn = &vo->g[k];
                        gn->bph += vf[o] * isr;
                        if (gn->bph >= 1.f)
                            gn->bph -= 1.f;
                        float tri = fabs_(gn->bph - 0.5f) * 4.f - 1.f;     /* (a triangle at the oscillator's pitch is enough for a signal at -66 dB) */
                        if (sine)
                            tri = sinsh(tri);
                        float tr = vbl[o] * tri;
                        yl += tr * vll[o];
                        yr += tr * vlr[o];
                        yb += tr;
                    }
                    mixl += yl;
                    mixr += yr;
                    busn[v] = hip(&vo->hp, yb, hp3c);
                    continue;
                }
                float mv = wb[v][0] * bus[0] + wb[v][1] * bus[1] + wb[v][2] * bus[2] + wb[v][3] * bus[3] + wtf[v] * tfb_s + wtc[v] +
                           wx[v] * ext, yl = 0.f, yr = 0.f, yb = 0.f;
                for (int k = 0; k < 2; k++) {
                    int o = 2 * v + k;
                    lira_gen *gn = &vo->g[k];
                    if (!vact[o]) {                       /* a closed oscillator beside an open one: the same stand-in */
                        gn->bph += vf[o] * isr;
                        if (gn->bph >= 1.f)
                            gn->bph -= 1.f;
                        float tb = fabs_(gn->bph - 0.5f) * 4.f - 1.f;
                        if (sine)
                            tb = sinsh(tb);
                        float tr = vbl[o] * tb;
                        yl += tr * vll[o];
                        yr += tr * vlr[o];
                        yb += tr;
                        continue;
                    }
                    float tri, sq;
                    float cpl = COUPLE * 0.5f * (sqv[(o + 1) & 7] + sqv[(o + 7) & 7]);
                    gen_tick(gn, vx[o] * (1.f + mv), cpl, sine, &tri, &sq);
                    sqv[o] = gn->s ? 1.f : -1.f;
                    if (sine)
                        tri = sinsh(tri);
                    if (vk[v] > 0.f) {                    /* the soft clipper: z (35 - 35 z^2 + 21 z^4 - 5 z^6) / 16 on |z| <= 1, flat beyond (smooth to the third derivative, no division) */
                        float z = clip_(tri * vk[v], -1.f, 1.f), z2 = z * z;
                        tri = z * (35.f + z2 * (-35.f + z2 * (21.f - 5.f * z2))) * vkn[v];
                    }
                    float wave = vat[v] * tri + vas[v] * sq;
                    float tr = wave * (vga[o] + vgb[o] * wave);
                    yl += tr * vll[o];                    /* what is heard: the voice's level fader, and its place left / right */
                    yr += tr * vlr[o];
                    yb += tr;                             /* what the other voices hear (FM): the voice itself, so a quiet voice can still be a modulator */
                }
                mixl += yl;
                mixr += yr;
                busn[v] = hip(&vo->hp, yb, hp3c);
            }
            for (int v = 0; v < 4; v++)
                bus[v] = busn[v];

            /* dual PT2399 delay with one shared feedback loop */
            float u = (stereo ? (mixl + mixr) * 0.5f : mixl) * 0.166667f;   /* (the middle; the side, left minus right, goes around the master stage) */
            u += l->ext_g * ext;
            ei++;
            float lin = u + rnd(l) * 0.0002f + fbl * l->loop_lp, wet = 0.f, yl0 = 0.f, yl1 = 0.f;   /* the noise is the delay's (converters, clock) */
            for (int i = 0; i < nlines; i++) {
                lira_line *ln = &l->line[i];
                float mv = delmod == 0 ? ln->self_lp * 0.5f : lfom;
                float yd = line_run(ln, lin, mv, k_ratio[i], d0[i], depth[i], dc);
                wet += yd;
                if (i == 0)
                    yl0 = yd;
                else
                    yl1 = yd;
            }
            wet *= nlines == 2 ? 0.5f : nlines == 1 ? 0.7f : 0.f;
            const float pw = ptanh(wet);
            lop(&l->loop_lp, pw, lp3k);                       /* op-amp / PT2399 overload and the loop's filter */
            float dly = nlines ? u + dm * (pw - u) : u;

            /* master: drive -> tanh (+ positive spike) -> mix with dry -> tanh -> tilt -> volume */
            float t = ptanh(hip(&l->hp1, dly * g, hp20c)), pw31 = 0.f;
            if (t > 0.f) {
                float t2 = t * t, t4 = t2 * t2, t8 = t4 * t4, t16 = t8 * t8;
                pw31 = t16 * t8 * t4 * t2 * t;
            }
            float dist = hip(&l->hp2, t + pw31 * 0.25f, hp10c) * ig;
            float yv = ptanh(dly * (1.f - dstm) + (dly * 0.1f + dist) * dstm);
            tfb_s = hip(&l->hp3, yv, hp3c);
            /* bass lifted a little, highs softer (manual) */
            float hh = 0.f;
            if (harm > 0.f) {                                 /* the bass band, rectified: its 2nd harmonic (the rectifier is linear in level, so it follows the dynamics) */
                float lb = lop(&l->hb2, lop(&l->hb1, yv, lp160), lp160);
                hh = harm * lop(&l->hbl, hip(&l->hbd, fabs_(lb), hp20c), lp350);
            }
            float yt = ptanh(lop(&l->hf, yv + bodyc * lop(&l->lf, yv, lp150) + hh, lp9k)) * vol;
            if (out_r) {                                      /* the side: the voices' spread and the two delay lines one to each side, softened like the master's top */
                float sd = (mixl - mixr) * 0.5f * 0.166667f;
                if (nlines)
                    sd += dm * (nlines == 2 ? 0.8f * 0.5f * wdt * (yl0 - yl1) : 0.f) - dm * sd;
                sd = lop(&l->side_lp, sd * vol, lp9k);
                *out++ = yt + sd;
                *out_r++ = yt - sd;
            } else {
                *out++ = yt;
            }
        }
        n -= m;
    }
    for (int v = 0; v < 4; v++)
        l->bus[v] = bus[v];
    for (int o = 0; o < 8; o++)
        l->sq[o] = sqv[o];
    l->total_fb = tfb_s;
    l->fol = fol;
}

void lira_process(lira_t *l, float *out, int n)
{
    if (l->out_r && l->width > 0.f) {
        process_(l, out, n, 1);
    } else {                                              /* mono: the right channel is a copy */
        process_(l, out, n, 0);
        if (l->out_r)
            for (int i = 0; i < n; i++)
                l->out_r[i] = out[i];
    }
}
