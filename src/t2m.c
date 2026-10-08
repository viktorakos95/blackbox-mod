/*
 * Trigger2MIDI on the Blackbox. Original 1010music Blackbox, firmware 3.1.9.
 *
 * Drum trigger detection on the audio input with MIDI notes / CCs out: a port of the gen~ codebox in
 * viktorakos95/trigger2midi (max/SPD_Trigger2MIDI_gen_codebox_FIXED_20260930.txt) and the Max objects around it.
 * t2m_ref.py is the codebox transcribed line for line; test_t2m.py runs both on the same signals and requires the same
 * MIDI out, event for event and sample for sample.
 *
 * The per-sample statements are the codebox's, in its order, with two changes that do not change any decision:
 *   - the coefficients the codebox recomputes every sample (exp of the knob values) are computed when a knob moves;
 *   - the dB values are never formed. Every use of them is a comparison, and 20 log10(a) - 20 log10(b) < x is
 *     a < b * 10^(x / 20), so fast_db / slow_db / diff_db become ratios against constants taken from the knobs.
 *   Sample counts are integers compared against the codebox's thresholds rounded up (n >= 96.00001 is n >= 97).
 * Display-only outputs (env, the crossing / accept times, peak_found_ms) are left out.
 *
 * Glue, as in the patch: on a hit v = round(1 + clip(shaped, 0, 1) * 126) clipped to 1..127; the CC goes first (Fixed
 * or v), then the note-on (Fixed or v; Note Out off = the patch's NO-NOTE). Note-off: Fixed length = after len_ms per
 * note-on (makenote); Dynamic = when the detector's note_active falls. Different from the patch on purpose: the
 * Dynamic note-off uses the channel setting (the patch has channel 1 there) and the pitch that was played (the patch
 * takes the menu's current one), and a new pitch while one is held ends the held one first.
 */
#include "t2m.h"

#define SR 48000.0

/* ---- exp / log (double; only run when a knob moves or once per hit) ---- */

typedef union { double d; uint64_t u; } dbits;

static double t2m_exp(double x)
{
    if (x < -700.0)
        return 0.0;
    if (x > 700.0)
        x = 700.0;
    double kf = x * 1.4426950408889634 + (x < 0 ? -0.5 : 0.5);
    int k = (int)kf;                                    /* nearest integer to x / ln 2 */
    double r = (x - k * 0.6931471803691238) - k * 1.9082149292705877e-10;   /* |r| <= ln2 / 2 */
    /* Taylor to r^12: error < 0.35^13 / 13! ~ 2e-16 */
    double p = 1.0 / 479001600.0;
    p = p * r + 1.0 / 39916800.0;
    p = p * r + 1.0 / 3628800.0;
    p = p * r + 1.0 / 362880.0;
    p = p * r + 1.0 / 40320.0;
    p = p * r + 1.0 / 5040.0;
    p = p * r + 1.0 / 720.0;
    p = p * r + 1.0 / 120.0;
    p = p * r + 1.0 / 24.0;
    p = p * r + 1.0 / 6.0;
    p = p * r + 0.5;
    p = p * r + 1.0;
    p = p * r + 1.0;
    if (k < -1022) {                                    /* keep the exponent field valid */
        p *= 1.0 / 4503599627370496.0;                  /* 2^-52 */
        k += 52;
    }
    dbits s = {.u = (uint64_t)(k + 1023) << 52};
    return p * s.d;
}

static double t2m_log(double x)
{
    if (x <= 0.0)
        return -1e300;
    dbits b = {.d = x};
    int e = (int)((b.u >> 52) & 0x7ff) - 1023;
    b.u = (b.u & 0x000fffffffffffffull) | 0x3ff0000000000000ull;   /* mantissa in [1, 2) */
    double m = b.d;
    if (m > 1.4142135623730951) {
        m *= 0.5;
        e += 1;
    }
    /* ln m = 2 atanh(s), s = (m - 1) / (m + 1), |s| <= 0.1716: series to s^21, error < 1e-17 */
    double s = (m - 1.0) / (m + 1.0), s2 = s * s;
    double p = 1.0 / 21;
    p = p * s2 + 1.0 / 19;
    p = p * s2 + 1.0 / 17;
    p = p * s2 + 1.0 / 15;
    p = p * s2 + 1.0 / 13;
    p = p * s2 + 1.0 / 11;
    p = p * s2 + 1.0 / 9;
    p = p * s2 + 1.0 / 7;
    p = p * s2 + 1.0 / 5;
    p = p * s2 + 1.0 / 3;
    p = p * s2 + 1.0;
    return 2.0 * s * p + e * 0.6931471805599453;
}

/* smallest integer n with n >= x (x >= 0) */
static uint32_t ceil_u(double x)
{
    uint32_t n = (uint32_t)x;
    return (double)n < x ? n + 1 : n;
}

static double maxd(double a, double b) { return a > b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }
static float minf(float a, float b) { return a < b ? a : b; }
static float absf(float x) { return x < 0.0f ? -x : x; }

/* ---- constants of the codebox ---- */
#define ENV_RELEASE_MS      80.0
#define DIP_FRACTION        0.5f
#define SLOW_MS             80.0
#define OFF_THRESH_DB       3.0
#define FLOOR_DB            -60.0
#define LN10                2.302585093
#define PI                  3.14159265358979
#define NOTE_OFF_QUIET_MS   60.0
#define NOTE_OFF_RESET_RATIO 1.5f
#define DYNAMIC_NOTE_MAX_MS 2000.0
#define FLUX_FC1            500.0
#define FLUX_FC2            3000.0
#define FLUX_ENV_MS         2.0

/* 20 log10(a) - 20 log10(b) with the codebox's LN10, compared with x, is a / b compared with this */
static float db_ratio(double x) { return (float)t2m_exp(x * LN10 / 20.0); }

static int same(const t2m_params *a, const t2m_params *b)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (uint32_t i = 0; i < sizeof *a; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

static void derive(t2m_state *t, const t2m_params *p)
{
    double threshold = 0.01 + ((double)p->thresh - 0.0) / (31.0 - 0.0) * (0.1 - 0.01);   /* scale 0 31 0.01 0.1 */
    double scan_ms = p->scan_ms, mask_ms = p->mask_ms;
    double retrig_cancel_ms = maxd(p->retrig, 1.0) * 20.0;
    double onset_speed_ms = maxd(p->speed_ms, 0.1);

    double scan_samples = scan_ms * 0.001 * SR;
    double onset_confirm_samples = onset_speed_ms * 0.001 * SR * 3.0;
    t->threshold = (float)threshold;
    t->scan_n = ceil_u(maxd(scan_samples, onset_confirm_samples));
    t->mask_n = ceil_u(mask_ms * 0.001 * SR);
    t->note_quiet_n = ceil_u(NOTE_OFF_QUIET_MS * 0.001 * SR);
    t->note_max_n = ceil_u(DYNAMIC_NOTE_MAX_MS * 0.001 * SR);
    t->quiet_min_n = ceil_u(maxd(SR * 0.001, scan_samples));

    t->fast_coeff = (float)t2m_exp(-1.0 / (onset_speed_ms * 0.001 * SR));
    t->slow_coeff = (float)t2m_exp(-1.0 / (SLOW_MS * 0.001 * SR));
    t->lp1_coeff = (float)t2m_exp(-2.0 * PI * FLUX_FC1 / SR);
    t->lp2_coeff = (float)t2m_exp(-2.0 * PI * FLUX_FC2 / SR);
    t->flux_coeff = (float)t2m_exp(-1.0 / (FLUX_ENV_MS * 0.001 * SR));
    t->rc_coeff = (float)t2m_exp(-1.0 / (retrig_cancel_ms * 0.001 * SR));   /* idle_coeff and rc_coeff */

    t->off_ratio = db_ratio(OFF_THRESH_DB);
    t->strict_ratio = db_ratio(p->strict_db);
    t->floor_lin = db_ratio(FLOOR_DB);                  /* slow_db > FLOOR_DB  <=>  max(slow, 1e-6) > this */
    t->len_n = (uint32_t)(p->len_ms * SR / 1000.0 + 0.5);

    t->cached = *p;
    t->have_cache = 1;
}

void t2m_reset(t2m_state *t)
{
    uint8_t *b = (uint8_t *)t;
    for (uint32_t i = 0; i < sizeof *t; i++)
        b[i] = 0;
    t->has_dipped = 1;
    t->best_ratio = 1e-10f;                             /* best_diff_db = -200 */
    t->held = -1;
}

static void emit(t2m_state *t, uint32_t frame, uint8_t status, uint8_t d1, uint8_t d2)
{
    if (t->n_events < T2M_EVENTS)
        t->events[t->n_events++] = (t2m_event){.frame = (uint16_t)frame, .status = status, .d1 = d1, .d2 = d2};
}

static void hit(t2m_state *t, const t2m_params *p, uint32_t frame)
{
    uint8_t ch = (uint8_t)((p->channel - 1) & 15);
    float x = t->last_shaped < 0.0f ? 0.0f : t->last_shaped > 1.0f ? 1.0f : t->last_shaped;
    int v = (int)(1.0f + x * 126.0f + 0.5f);
    v = v < 1 ? 1 : v > 127 ? 127 : v;
    if (p->cc_on) {
        int c = p->cc_fixed < 1 ? 1 : p->cc_fixed > 127 ? 127 : p->cc_fixed;
        emit(t, frame, 0xB0 | ch, p->cc_num & 127, (uint8_t)(p->cc_mode ? c : v));
    }
    if (p->note_on) {
        uint8_t note = p->note & 127, vel = (uint8_t)(p->vel_mode ? p->vel_fixed : v);
        if (p->len_mode) {
            if (t->n_offs == sizeof t->offs / sizeof t->offs[0]) {       /* full: end the oldest now */
                emit(t, frame, 0x80 | ch, t->offs[0].note, 0);
                for (uint32_t i = 1; i < t->n_offs; i++)
                    t->offs[i - 1] = t->offs[i];
                t->n_offs--;
            }
            t->offs[t->n_offs].left = t->len_n;
            t->offs[t->n_offs].note = note;
            t->n_offs++;
        } else {
            if (t->held >= 0 && t->held != note)
                emit(t, frame, 0x80 | ch, (uint8_t)t->held, 0);
            t->held = note;
        }
        emit(t, frame, 0x90 | ch, note, vel);
    }
}

uint32_t t2m_process(t2m_state *t, const t2m_params *p, const float *in, uint32_t n)
{
    if (!t->have_cache || !same(&t->cached, p))
        derive(t, p);
    t->n_events = 0;
    uint8_t ch = (uint8_t)((p->channel - 1) & 15);
    const float threshold = t->threshold, sens = p->sens;
    const float fast_coeff = t->fast_coeff, slow_coeff = t->slow_coeff, lp1_coeff = t->lp1_coeff,
                lp2_coeff = t->lp2_coeff, flux_coeff = t->flux_coeff, rc_coeff = t->rc_coeff,
                off_ratio = t->off_ratio, floor_lin = t->floor_lin, note_reset = threshold * NOTE_OFF_RESET_RATIO;
    const uint32_t quiet_min_n = t->quiet_min_n, note_quiet_n = t->note_quiet_n, note_max_n = t->note_max_n;

    /* the codebox's History in locals for the block (the compiler cannot keep them in registers through t->) */
    uint32_t state = t->state, timer = t->timer, has_dipped = t->has_dipped, quiet_samples = t->quiet_samples,
             note_quiet_samples = t->note_quiet_samples, note_active_samples = t->note_active_samples,
             note_active = t->note_active;
    float peak_velo = t->peak_velo, dynamic_thresh = t->dynamic_thresh, fast_env = t->fast_env,
          slow_env = t->slow_env, lp1 = t->lp1, lp2 = t->lp2, band_low_env = t->band_low_env,
          band_mid_env = t->band_mid_env, band_high_env = t->band_high_env, peak_band_low = t->peak_band_low,
          peak_band_mid = t->peak_band_mid, peak_band_high = t->peak_band_high, best_ratio = t->best_ratio;

    for (uint32_t i = 0; i < n; i++) {
        float rect = absf(in[i] * sens);

        /* Fixed-length note-offs due now (makenote) */
        for (uint32_t k = 0; k < t->n_offs;) {
            if (--t->offs[k].left == 0) {
                emit(t, i, 0x80 | ch, t->offs[k].note, 0);
                for (uint32_t j = k + 1; j < t->n_offs; j++)
                    t->offs[j - 1] = t->offs[j];
                t->n_offs--;
            } else {
                k++;
            }
        }

        fast_env = rect + (fast_env - rect) * fast_coeff;
        slow_env = rect + (slow_env - rect) * slow_coeff;
        float fast = maxf(fast_env, 0.000001f), slow = maxf(slow_env, 0.000001f);

        if (rect < dynamic_thresh * DIP_FRACTION)
            has_dipped = 1;

        if (fast < slow * off_ratio) {                  /* diff_db < OFF_THRESH_DB */
            quiet_samples++;
            if (quiet_samples >= quiet_min_n)
                has_dipped = 1;
        } else {
            quiet_samples = 0;
        }

        uint32_t was_active = note_active;
        if (rect < threshold)
            note_quiet_samples++;
        else if (rect >= note_reset)
            note_quiet_samples = 0;
        if (note_active && note_quiet_samples >= note_quiet_n) {
            note_active = 0;
            note_active_samples = 0;
        }
        if (note_active) {
            note_active_samples++;
            if (note_active_samples >= note_max_n) {
                note_active = 0;
                note_active_samples = 0;
            }
        } else {
            note_active_samples = 0;
        }

        lp1 = rect + (lp1 - rect) * lp1_coeff;
        lp2 = rect + (lp2 - rect) * lp2_coeff;
        float band_low = lp1, band_mid = lp2 - lp1, band_high = rect - lp2;
        float al = absf(band_low), am = absf(band_mid), ah = absf(band_high);
        band_low_env = al + (band_low_env - al) * flux_coeff;
        band_mid_env = am + (band_mid_env - am) * flux_coeff;
        band_high_env = ah + (band_high_env - ah) * flux_coeff;

        if (state == 0) {
            if (rect > threshold && slow > floor_lin && has_dipped) {
                timer = 0;
                peak_velo = rect;
                has_dipped = 0;
                state = 1;
                best_ratio = fast / slow;
                peak_band_low = band_low_env;
                peak_band_mid = band_mid_env;
                peak_band_high = band_high_env;
            } else {
                dynamic_thresh = threshold + (dynamic_thresh - threshold) * rc_coeff;
            }
        } else if (state == 1) {
            timer++;
            if (rect > peak_velo) {
                peak_band_low = band_low_env;
                peak_band_mid = band_mid_env;
                peak_band_high = band_high_env;
            }
            peak_velo = maxf(peak_velo, rect);
            if (fast > best_ratio * slow)               /* diff_db > best_diff_db */
                best_ratio = fast / slow;
            if (timer >= t->scan_n)
                state = 2;
        } else if (state == 2) {
            float margin = (peak_velo - dynamic_thresh) / maxf(dynamic_thresh, 0.000001f);
            float max_band = maxf(maxf(peak_band_low, peak_band_mid), peak_band_high);
            float min_band = minf(minf(peak_band_low, peak_band_mid), peak_band_high);
            float spectral_flatness = min_band / maxf(max_band, 0.000001f);
            if (peak_velo > dynamic_thresh) {
                if (best_ratio <= t->strict_ratio
                    || (margin < maxf(p->spec_range, 0.0f) && spectral_flatness < maxf(p->spec_strict, 0.0f))) {
                    state = 0;
                } else {
                    float pv = peak_velo > 1.0f ? 1.0f : peak_velo;     /* > threshold > 0 */
                    t->last_shaped = (float)t2m_exp(p->curve * t2m_log(pv));   /* pow(clip(peak, 0, 1), curve) */
                    dynamic_thresh = peak_velo;
                    timer = 0;
                    state = 3;
                    note_active = 1;
                    note_quiet_samples = 0;
                    note_active_samples = 0;
                    hit(t, p, i);
                }
            } else {
                state = 0;
            }
        } else {
            timer++;
            dynamic_thresh = dynamic_thresh * rc_coeff;
            if (timer >= t->mask_n)
                state = 0;
        }

        if (was_active && !note_active && t->held >= 0) {   /* Dynamic length: the hit has died away */
            emit(t, i, 0x80 | ch, (uint8_t)t->held, 0);
            t->held = -1;
        }
    }

    t->state = state, t->timer = timer, t->has_dipped = has_dipped, t->quiet_samples = quiet_samples;
    t->note_quiet_samples = note_quiet_samples, t->note_active_samples = note_active_samples;
    t->note_active = note_active;
    t->peak_velo = peak_velo, t->dynamic_thresh = dynamic_thresh, t->fast_env = fast_env, t->slow_env = slow_env;
    t->lp1 = lp1, t->lp2 = lp2, t->band_low_env = band_low_env, t->band_mid_env = band_mid_env;
    t->band_high_env = band_high_env, t->peak_band_low = peak_band_low, t->peak_band_mid = peak_band_mid;
    t->peak_band_high = peak_band_high, t->best_ratio = best_ratio;
    return t->n_events;
}
