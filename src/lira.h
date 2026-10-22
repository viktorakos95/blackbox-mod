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

#define LIRA_DL_LEN 44032   /* the PT2399's 44,000 entries plus margin */

/* Parameter ids: the 47 controls of the Pd patch in the same order as its parameters.txt (all 0..127 unless noted). */
enum {
    LIRA_TUNE1, LIRA_TUNE2, LIRA_TUNE3, LIRA_TUNE4, LIRA_TUNE5, LIRA_TUNE6, LIRA_TUNE7, LIRA_TUNE8, /* osc pitch */
    LIRA_FAST12, LIRA_FAST34, LIRA_FAST56, LIRA_FAST78,   /* 0/1: touch envelope 100/100 ms (1) or 200 ms / 8 s (0) */
    LIRA_SHARP12, LIRA_SHARP34, LIRA_SHARP56, LIRA_SHARP78,   /* triangle <-> square */
    LIRA_MOD12, LIRA_MOD34, LIRA_MOD56, LIRA_MOD78,   /* FM depth */
    LIRA_SOURCE12, LIRA_SOURCE34, LIRA_SOURCE56, LIRA_SOURCE78,   /* FM source: 0 self (neighbour voices), 1 off, 2 LFO/feedback, 3 ext, 4..7 a pair, 8 the master output */
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
    LIRA_EXT,         /* 0..127: external audio in level (the Blackbox's line inputs), mixed in ahead of the delay */
    LIRA_WAVE,        /* 0 the oscillator's triangle (the plugin's), 1 a sine shaped from it */
    LIRA_LVL1, LIRA_LVL2, LIRA_LVL3, LIRA_LVL4, LIRA_LVL5, LIRA_LVL6, LIRA_LVL7, LIRA_LVL8,   /* per-oscillator level (squared taper) */
    LIRA_NPARAMS
};

typedef struct { float cur, tgt, step; int n; } lira_ramp;

/* One tone generator: an HEF4093 Schmitt-trigger relaxation oscillator (timing capacitor v between two thresholds, gate
   output s), its envelope (RC), vibrato and per-unit tolerances. See docs/LIRA-analog-notes.md. */
typedef struct {
    float v, kt, vp, vn, mid, inv_half;   /* capacitor voltage (rail = 1), ln-sum of the half periods, thresholds */
    int s;                                /* gate output high */
    float sqp, trip;                      /* previous-sample square (PolyBLEP, one sample late) and triangle */
    float linp, lr, lf;                   /* the triangle linear in time (for the sine), and 1 / log2 of each half's voltage ratio */
    float u, lrt, lft;                    /* how far through the half period (0..1), integrated; and 1 / ln of each half's voltage ratio: its rate per unit of x */
    float ve, drive;                      /* envelope voltage, touch drive (0 = not touched) */
    float t_slow, t_fast;                 /* release time constants of this voice, s */
    float trim, leak;                     /* level trim, bleed gain at 1 kHz */
    float leak_f;                         /* bleed at the current pitch (refreshed every 8th block) */
    float vib_ph, vib_rate, vib_depth;
    float bph;                            /* phase of the cheap stand-in that carries a closed oscillator's bleed */
    lira_ramp tune;                       /* Hz */
    lira_ramp lvl;                        /* this oscillator's level (a mixer fader) */
} lira_gen;

typedef struct {
    lira_gen g[2];
    lira_ramp sharp, mod;
    float hp;
    int fast;
    int follow;                           /* Touch = Follow: the envelope follows the external input's level, not the sensors */
} lira_voice;

/* One PT2399: 44,000-entry memory clocked by a VCO. Above ratio 1 (clock faster than the audio rate) it is a tap; below,
   it steps at the clock and the input goes through an adaptive 1-bit delta codec. */
typedef struct {
    int16_t *buf;                         /* the memory, 16-bit (half the RAM of floats; the PT2399 is a ~12-bit part): value * DL_SCALE */
    int len, w;
    float acc, est, stp;
    float dpos;                           /* the tap's delay in samples as it is now (gliding to the clock's value) */
    int pbit;
    float hp_x, in1, in2, out1, out2, self_lp;
    lira_ramp time, mod;                  /* seconds at the clock set by the knob; relative clock modulation depth */
} lira_line;

typedef struct {
    float params[LIRA_NPARAMS];
    float sr;
    int n23;
    uint32_t rng, cb;    /* cb: control block counter */
    lira_voice v[4];
    lira_ramp hold[2];
    lira_ramp vibsw;
    float bus[4];        /* voice outputs (high-passed) read by the others, one sample late */
    float total_fb;      /* master output, high-passed, one sample late */
    float pa, pb;        /* hyper-LFO phases */
    float sqa_s, sqb_s;  /* hyper-LFO square edges, slewed */
    float sq[8];         /* gate outputs (+-1), for the neighbour coupling */
    lira_ramp lfo_a, lfo_b;
    lira_line line[2];
    float loop_lp;       /* shared delay feedback path */
    lira_ramp fbline, dmix, drv_g, dst, vol;
    float hp1, hp2, hp3, lf, hf;
    const float *ext_l, *ext_r;   /* the block's input, set by the caller before lira_process */
    int ext_n;                    /* frames valid in them */
    float ext_g;
    float fol;                    /* envelope follower of the external input */
    int shed;                     /* load shedding set by the caller: 0 both delay lines, 1 one line, 2 no delay */
    int wave_on;                  /* the WAVE the oscillators' triangles were last kept for (gen_sync) */
    float width, drift;           /* stereo width and slow pitch wander (0..1); width 0 runs the cheaper mono path */
    float bodyc;                  /* the bass lift (a gain; 0.41, fixed) */
    float pan[8];                 /* each oscillator's place in the stereo image (-1..1, random per unit, scaled by width) */
    float *out_r;                 /* the right channel, set by the caller before lira_process (NULL: mono, as the pads use it) */
    float dr[8];                  /* the oscillators' pitch wander, relative */
    float side_lp;                /* the side signal's top-end filter */
} lira_t;

int lira_size(void);
/* dl1, dl2: PT2399 memories of `len` >= LIRA_DL_LEN 16-bit entries each (caller-owned, zeroed). */
void lira_init(lira_t *l, float sample_rate, int16_t *dl1, int16_t *dl2, int len, uint32_t seed);
/* lira_init without clearing the delay buffers (the caller clears them, e.g. a slice per audio block) */
void lira_reset(lira_t *l, float sample_rate, int16_t *dl1, int16_t *dl2, int len, uint32_t seed);
/* every parameter ramp to its target at once */
void lira_settle(lira_t *l);
void lira_param(lira_t *l, int id, float value);
/* The value of LIRA_TUNE1 + o (0..127) that makes oscillator o sound the MIDI note `midi` (fractions allowed) when its pitch
   knobs are at LIRA_PITCH_UNITY; the note should lie in 33 .. 93, which every oscillator reaches. */
#define LIRA_PITCH_UNITY 63.1809f
float lira_tune_for_midi(int o, float midi);
/* the range of MIDI notes oscillator o can reach at LIRA_PITCH_UNITY */
void lira_tune_range(int o, float *lo, float *hi);
/* tempo sync (the caller works out hz, the phase 0..1 within the cycle or -1, and seconds) */
void lira_lfo_sync(lira_t *l, int which, float hz, float phase);
void lira_delay_sync(lira_t *l, int which, float sec);
void lira_key(lira_t *l, int sensor /* 0..7 */, int on);
/* touch with a pressure/velocity: drive 0 = released, 0..1 = how far the envelope opens and how fast */
void lira_touch(lira_t *l, int sensor, float drive);
void lira_process(lira_t *l, float *out, int n);
/* The page's tone: width = stereo spread of the pairs and the two delay lines (0 = mono, and the right channel is a copy: the cheap path);
   drift = how far each oscillator wanders (up to ~6 cents, a few seconds a turn). */
void lira_tone(lira_t *l, float width, float drift);

#endif
