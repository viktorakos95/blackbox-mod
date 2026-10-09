/* SAMPLR: a sample played from the touch screen (waveform, slicer, tape, arpeggiator, granular). The state lives in the looper's
 * spare effect memory block 9 (looper_scratch(9)); samplr_run mixes the voices into the Out 1 bus after the looper. */
#pragma once
#include <stdint.h>

#define SM_COLS   150            /* waveform overview columns */
#define SM_VOICES 4              /* one per finger */
#define SM_ARPV   4              /* voices of the arpeggiator's own notes (after the fingers') */
#define SM_LAYERS 3              /* gesture overdub layers */
#define SM_LFING  4              /* fingers per layer */
#define SM_LATV   8              /* voices for latched slice loops (not tied to a finger) */
#define SM_LTBASE (SM_VOICES + SM_ARPV)
#define SM_LBASE  (SM_LTBASE + SM_LATV)
#define SM_NV     (SM_LBASE + SM_LAYERS * SM_LFING)
#define SM_EVENTS 1000           /* gesture events per layer */
#define SM_GRAINS 6              /* per finger */
#define SM_SPOTS  8              /* arpeggiator spots */
#define SM_CUTS   64             /* slice points: up to 64 slices */
#define SM_OWIN   2048           /* transient search windows */
#define SM_PADS   16

enum { SM_SLICER, SM_TAPE, SM_ARP, SM_GRAIN, SM_MODES };
enum { SM_PAT_UP, SM_PAT_DOWN, SM_PAT_UPDN, SM_PAT_RND, SM_PAT_ORDER, SM_PATS };

struct smspot {
    int32_t pos;
    int8_t st;
    uint8_t used, owner, vol;               /* owner: the finger holding it, or 0xff once latched; vol: from the finger's height */
    int32_t end;                            /* with SNAP: the end of its slice (a note never runs into the next slice), else 0 */
};

struct smgrain {
    int32_t ip, age, len, delay;
    float frac, rate, gl, gr;
    uint8_t on, cont, _p[2];                /* cont: the envelope shape */
};

struct smvoice {
    volatile uint32_t cmd, seen;            /* trigger sequence: the GUI task bumps cmd after writing the c_ fields */
    int32_t c_pos, c_start, c_end;
    float c_rate, c_gain, c_q;              /* c_q: start on the next grid line of this many beats (0 = at once) */
    float c_rep, rep;                       /* rep: restart the slice on every grid line of this many beats while held (0 = no) */
    uint8_t c_loop, c_gate, held, rel;
    uint8_t on, loop, slice, vmode, c_mode, owner, _w[2];   /* vmode: the mode that played it; c_mode: ... that triggered it; owner: for a latched loop, 0 = played live, else the gesture layer + 1 */
    int32_t ipos, start, end;
    float frac, rate, gain, env;            /* rate and gain are rewritten live by the GUI (tape) */
    float rate_s, g_prev;                   /* the rate the audio follows (slewed), the gain at the end of the last block (ramped to the new one) */
    int32_t wait, c_wait;                         /* frames into the block where the note starts */
    /* arpeggiator: the spot this finger holds (-1 none) */
    int8_t a_sp;
    uint8_t _a[3];
    /* granular cloud */
    uint8_t g_on;
    uint8_t _g[3];
    int32_t g_centre, g_size;
    float g_acc, g_warp, g_warp2;            /* g_warp: smooth random walk of the scan / pan (WARP spray) */
    uint8_t g_seq, _gs[3];                  /* step of the pitch pattern */
    struct smgrain g[SM_GRAINS];
};

/* Gesture recorder: touch events of one layer, time in 64-frame units from the loop start. w = kind (2 bits) | finger (2) | mode (2) | fx (10) | fy (10) | latch (1). */
struct smev {
    uint16_t t, _p;
    uint32_t w;
};

struct smgest {                              /* lives in effect block 10 */
    uint16_t n[SM_LAYERS], _p;
    struct smev ev[SM_LAYERS][SM_EVENTS];
};

struct sm {
    uint32_t magic;
    uint8_t mode, gate, nslice, npads;
    uint8_t sel, ov_ok, mono;
    uint8_t dbg[3];                         /* pads with rec[8] set, with an id, with a live sample */
    uint8_t pad_row[SM_PADS], pad_col[SM_PADS];
    uint16_t pad_id[SM_PADS];
    int32_t id, len, hz;
    float ratio, vol;
    uint32_t ov_t;
    int16_t fx0[SM_NV];                     /* where each finger went down (tape, slice points) */
    uint8_t qi, div, pat, latchm, atk, rel;  /* quantize (0 off, 1 1/4, 2 1/8, 3 1/16; in ARP: snap to slices), arp / grain rate, arp pattern, hold (a bit per mode), attack / release choice */
    uint8_t g_sz, g_dry;                    /* GRAIN: grain size scale (0 as the finger says, 1 a quarter, 2 a sixteenth), the normal loop under the grains (0 off, 1 25 %, 2 50 %, 3 100 %) */
    int32_t dry_pos;                        /* GRAIN: where that loop is */
    float dry_env, dry_frac;
    uint8_t g_cont, g_warpmode, g_ppat;     /* GRAIN: envelope contour (sine, down ramp, up ramp, flat), spray type (0 random, 1 warp), pitch pattern (0 off, 1..4 a scale) */
    int8_t g_drift;                         /* GRAIN: scan speed -8..8 (x 1/4 of real time; 0 = the cloud stays where it is) */
    uint8_t gfree, a_last, sp_next, loopm;  /* loopm: SLICER loops its slice while held (or latched) */     /* grain rate free (grains per second) instead of the grid; the spot played last; next spot to replace */
    uint16_t a_step, _q2;
    float scat, sph, dens;                  /* grain scatter 0..1; free-running phase in frames; free grain density per second */
    uint32_t tick, pf_t;
    uint32_t t_last, t_sum, t_psum, t_pmax, t_per; /* cycle counter at the last run; cycles spent in this report, the block periods summed, the longest run */
    uint8_t n_voices, n_grains;             /* playing now (for the readout) */
    uint16_t t_n, t_peak, t_avg_shown, t_peak_shown, load;   /* load: the last block alone */
    uint32_t auto_t;                        /* when AUTO last ran */
    uint8_t auto_found, _a2[3];   /* load of samplr_run in per mille of the block period */                    /* blocks run; when the last load was asked for */
    struct smspot spot[SM_SPOTS];
    uint32_t rnd;
    int16_t kacc[4];                        /* knob counts not yet turned into a step */
    struct smgest *gest;
    int32_t g_len, g_pos;                   /* gesture loop length and position, frames (g_pos < 0: the loop starts that many frames into this block) */
    uint8_t g_bars, g_run, g_armed, g_layers;   /* bars (1 2 4 8); the timeline runs; waiting for: 1 a bar line to record the first layer, 2 the loop start to record the next, 3 a bar line to play */
    int8_t g_rec;                           /* the layer being recorded (-1 none) */
    uint8_t g_lmode[SM_LAYERS], g_ldown[SM_LAYERS];
    uint16_t g_rp[SM_LAYERS], g_lastmv[SM_VOICES];
    int16_t lfx[SM_VOICES], lfy[SM_VOICES]; /* where each live finger last was (a take can start with fingers already down) */
    uint16_t filled;                        /* overview columns complete */
    uint8_t ofill[SM_COLS];
    float omn[SM_COLS], omx[SM_COLS];
    int8_t ov[2][SM_COLS];                  /* per column: lowest and highest sample, -127..127 */
    struct smvoice v[SM_NV];
    int32_t cut[SM_CUTS + 1];               /* slice i = frames cut[i] .. cut[i + 1] */
    int8_t drag[SM_NV];                 /* finger -> the slice point it moves (-1 none; 100: the press removed a latched spot, ignore the finger) */
    uint8_t dmoved[SM_NV];              /* the dragged point moved (else a tap on it deletes it) */
    int8_t trans;                           /* transpose, semitones -48..48 */
    uint8_t ypit;                           /* bit per mode: finger height = pitch */
    uint8_t _t[2];
    float il[256], ir[256];                 /* one block of the source, interpolated at the voice's positions */
    float oenv[SM_OWIN];                    /* transient search scratch (GUI task only) */
};

struct sm *samplr(void);                    /* 0 until the looper's memory is up */
void samplr_run(float *bl, float *br, int n);
void samplr_refresh(unsigned ticks);        /* call while the tab shows: fills the waveform as a streamed sample loads */
void samplr_enter(void);                    /* the tab was opened: look at the pads again */
void samplr_leave(void);                    /* the tab was left: let go of everything */
void samplr_select(int delta);              /* previous / next loaded pad sample */
void samplr_set_mode(int m);
void samplr_toggle_gate(void);
void samplr_set_slices(int n);
void samplr_cycle(int what);                /* 0 quantize / snap / sync-free, 1 arp pattern, 2 latch, 3 height = pitch, 4 find transients, 5 grain contour, 6 spray type, 7 pitch pattern, 8 grain size scale, 9 dry loop level */
void samplr_trans(int what);                /* transpose: +-1, +-12, 0 = back to 0 */
void samplr_knob(int knob, int counts);     /* knobs 0..3 per mode (0 = volume); recorded into a take */
void samplr_touch(int kind, int id, int fx, int fy);   /* kind 0 down, 1 move, 2 up; fx, fy 0..1023 inside the waveform (recorded when a layer is recording) */
void samplr_gest(int what);                 /* 0 REC, 1 PLAY / STOP, 2 UNDO, 3 CLR, 4 LEN, 5 PLAY, 6 STOP (the hardware buttons) */
void samplr_name(char *out, int max);       /* the selected sample's name for the page */
void samplr_info(char *out);                /* the mode's settings as text for the top bar */
const char *samplr_pat_name(int p);
const char *samplr_cont_name(int c);
const char *samplr_ppat_name(int p);
uint32_t samplr_sig(void);
