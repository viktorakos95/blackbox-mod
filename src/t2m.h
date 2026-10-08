/* Trigger2MIDI on the Blackbox: drum trigger detection on the audio input, MIDI notes / CCs out. See src/t2m.c. */
#pragma once
#include <stdint.h>

#define T2M_EVENTS 32

/* One input's settings, the Max patch's controls with its ranges. */
typedef struct {
    float sens;          /* Sensitivity (gain) 0.1 .. 10 */
    float thresh;        /* Threshold knob 0 .. 31 (0.01 .. 0.1 of full scale after the gain) */
    float retrig;        /* Retrigger Cancel 1 .. 16 */
    float mask_ms;       /* Mask Time 15 .. 64 */
    float scan_ms;       /* Scan Time 0 .. 4 */
    float curve;         /* Velocity Curve 0.1 .. 1.5 */
    float strict_db;     /* Onset Strictness 0 .. 20 dB */
    float speed_ms;      /* Onset Speed 0.2 .. 10 ms */
    float spec_strict;   /* Anti-bleed Strict 0 .. 0.6 (0 = off) */
    float spec_range;    /* Anti-bleed Range 0 .. 1 (0 = off) */
    uint16_t len_ms;     /* Note Length Ms 5 .. 2000 (Fixed length) */
    uint8_t note_on;     /* Note Out */
    uint8_t note;        /* Target MIDI note 0 .. 127 */
    uint8_t vel_mode;    /* 0 Dynamic, 1 Fixed */
    uint8_t vel_fixed;   /* 1 .. 127 */
    uint8_t len_mode;    /* 0 Dynamic (note-off when the hit dies away), 1 Fixed (len_ms) */
    uint8_t cc_on;       /* CC Out */
    uint8_t cc_num;      /* 0 .. 127 */
    uint8_t cc_mode;     /* 0 Dynamic, 1 Fixed */
    uint8_t cc_fixed;    /* 1 .. 127 */
    uint8_t channel;     /* 1 .. 16 */
} t2m_params;

typedef struct {
    uint16_t frame;      /* sample inside the block */
    uint8_t status, d1, d2;
} t2m_event;

typedef struct {
    /* the codebox's History (only the ones that decide something; display-only outputs are left out) */
    uint32_t state, timer;
    float peak_velo, dynamic_thresh;
    uint32_t has_dipped;
    uint32_t quiet_samples, note_quiet_samples, note_active_samples;
    float fast_env, slow_env;
    uint32_t note_active;
    float lp1, lp2, band_low_env, band_mid_env, band_high_env, peak_band_low, peak_band_mid, peak_band_high;
    float best_ratio;                /* best_diff_db as fast / slow (dB = 20 log10 of it) */
    float last_shaped;
    /* glue */
    int16_t held;                    /* Dynamic length: the pitch sounding, -1 none */
    uint8_t n_offs;
    struct { uint32_t left; uint8_t note; } offs[16];   /* Fixed length: pending note-offs */
    /* derived from the params (recomputed when they change) */
    t2m_params cached;
    uint32_t have_cache;
    float threshold, fast_coeff, slow_coeff, lp1_coeff, lp2_coeff, flux_coeff, rc_coeff;
    float off_ratio, strict_ratio, floor_lin;
    uint32_t scan_n, quiet_min_n, mask_n, note_quiet_n, note_max_n, len_n;
    /* this block's MIDI */
    uint32_t n_events;
    t2m_event events[T2M_EVENTS];
} t2m_state;

void t2m_reset(t2m_state *t);
/* One block of one input (full scale +-1.0). Fills t->events / t->n_events. */
uint32_t t2m_process(t2m_state *t, const t2m_params *p, const float *in, uint32_t n);
