/*
 * LIRA-8 engine: a C port of Mike Moreno DSP's LIRA-8 (Pure Data), an emulation of the SOMA Lab Lyra-8 drone synth.
 * Original work: copyright Miguel Moreno, "Standard Improved BSD License", see THIRD_PARTY.md and src/lira/LICENSE.
 *
 * One instance = one LIRA-8: 8 triangle/square oscillators in 4 voices (cross-FM through "delay buses"), a hyper-LFO
 * (two phasors), a dual modulated delay with compressor/expander in the feedback path, and a drive/saturation master.
 * Mono out. Plain C, no libm, no malloc: the caller owns the struct and the two delay buffers.
 */
#ifndef LIRA_H
#define LIRA_H

#include <stdint.h>

/* Parameter ids: the 47 controls of the Pd patch in the same order as its parameters.txt (all 0..127 unless noted). */
enum {
    LIRA_TUNE1, LIRA_TUNE2, LIRA_TUNE3, LIRA_TUNE4, LIRA_TUNE5, LIRA_TUNE6, LIRA_TUNE7, LIRA_TUNE8, /* osc pitch */
    LIRA_FAST12, LIRA_FAST34, LIRA_FAST56, LIRA_FAST78,   /* 0/1: touch envelope 100/100 ms (1) or 200 ms / 8 s (0) */
    LIRA_SHARP12, LIRA_SHARP34, LIRA_SHARP56, LIRA_SHARP78,   /* triangle <-> square */
    LIRA_MOD12, LIRA_MOD34, LIRA_MOD56, LIRA_MOD78,   /* FM depth */
    LIRA_SOURCE12, LIRA_SOURCE34, LIRA_SOURCE56, LIRA_SOURCE78,   /* FM source: 0 self (neighbour voices), 1 off, 2 LFO/feedback */
    LIRA_PITCH1234, LIRA_PITCH5678,   /* global pitch of each half */
    LIRA_HOLD1234, LIRA_HOLD5678,     /* drone level with no touch */
    LIRA_SWITCH,      /* 0/1: which neighbour voice drives the FM in "self" */
    LIRA_TOTAL_FB,    /* 0/1: "lfo" source takes the master output instead of the square LFO */
    LIRA_VIBRATO,
    LIRA_DRV, LIRA_DST_MIX, LIRA_VOL,
    LIRA_F_A, LIRA_F_B,   /* hyper-LFO rates */
    LIRA_ANDOR,       /* 0/1: LFO combine: 0 = product, 1 = sum */
    LIRA_LINK,        /* 0/1: LFO A modulates the rate of LFO B */
    LIRA_TIME1, LIRA_TIME2, LIRA_FEEDBACK, LIRA_DEL_MIX, LIRA_MOD1, LIRA_MOD2,
    LIRA_DEL_MOD,     /* 0 self, 1 off, 2 LFO */
    LIRA_LFO_WAV,     /* 0 triangle, 1 square */
    LIRA_QUANTIZE,    /* 0/1 */
    LIRA_NPARAMS
};

typedef struct { float cur, tgt, step; int n; } lira_ramp;
typedef struct { float ph, last, pwh, lp; } lira_osc;
typedef struct { float env, atk, rel; } lira_comp;
typedef struct { float acc, prev; int cnt, init; lira_ramp red; } lira_exp;

typedef struct {
    lira_osc osc[2];
    lira_ramp tune[2], env[2], sharp, mod, vib;
    float vib_ph, vib_rate, hp;
    int key[2];
} lira_voice;

typedef struct {
    float *buf;
    int len, w;
    lira_ramp time, mod;
    float lp_in, hp_x, tap_lp;
    lira_comp comp;
    lira_exp ex;
} lira_line;

typedef struct {
    float params[LIRA_NPARAMS];
    float sr;
    int n23, n11;
    uint32_t rng;
    lira_voice v[4];
    lira_ramp hold[2];
    float bus[4];        /* voice outputs (high-passed) read by the others, one sample late */
    float total_fb;      /* master output, high-passed, one sample late */
    float pa, pb, sqa, sqb;
    lira_ramp lfo_a, lfo_b;
    lira_line line[2];
    lira_ramp fbline, dmix, drv_g, dst, vol;
    float hp1, hp2, hp3;
} lira_t;

int lira_size(void);
/* dl1, dl2: delay buffers of `len` samples each (caller-owned). The delay-time knob maps up to len/sr (max 5.94 s). */
void lira_init(lira_t *l, float sample_rate, float *dl1, float *dl2, int len, uint32_t seed);
void lira_param(lira_t *l, int id, float value);
void lira_key(lira_t *l, int sensor /* 0..7 */, int on);
void lira_process(lira_t *l, float *out, int n);

#endif
