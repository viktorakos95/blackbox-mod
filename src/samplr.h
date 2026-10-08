/* SAMPLR: a sample played from the touch screen (waveform, slicer, tape, arpeggiator, granular). The state lives in the looper's
 * spare effect memory block 9 (looper_scratch(9)); samplr_run mixes the voices into the Out 1 bus after the looper. */
#pragma once
#include <stdint.h>

#define SM_COLS   150            /* waveform overview columns */
#define SM_VOICES 4              /* one per finger */
#define SM_GRAINS 6              /* per finger */
#define SM_SPOTS  8              /* arpeggiator spots */
#define SM_TMP    4096           /* source frames one read takes per block (15x pitch up of 256 frames + margin); tl / tr live in effect blocks 10 / 11 */
#define SM_CUTS   64             /* slice points: up to 64 slices */
#define SM_OWIN   2048           /* transient search windows */
#define SM_PADS   16

enum { SM_SLICER, SM_TAPE, SM_ARP, SM_GRAIN, SM_MODES };
enum { SM_PAT_UP, SM_PAT_DOWN, SM_PAT_UPDN, SM_PAT_RND, SM_PAT_ORDER, SM_PATS };

struct smspot {
    int32_t pos;
    int8_t st;
    uint8_t used, owner, _p;                /* owner: the finger holding it, or 0xff once latched */
};

struct smgrain {
    int32_t ip, age, len, delay;
    float frac, rate, gl, gr;
    uint8_t on, _p[3];
};

struct smvoice {
    volatile uint32_t cmd, seen;            /* trigger sequence: the GUI task bumps cmd after writing the c_ fields */
    int32_t c_pos, c_start, c_end;
    float c_rate, c_gain, c_q;              /* c_q: start on the next grid line of this many beats (0 = at once) */
    uint8_t c_loop, c_gate, held, rel;
    uint8_t on, loop, slice, wait_hi;
    int32_t ipos, start, end;
    float frac, rate, gain, env;            /* rate and gain are rewritten live by the GUI (tape) */
    int32_t wait, c_wait;                         /* frames into the block where the note starts */
    /* arpeggiator: the spot this finger holds (-1 none) */
    int8_t a_sp;
    uint8_t _a[3];
    /* granular cloud */
    uint8_t g_on;
    uint8_t _g[3];
    int32_t g_centre, g_size;
    float g_acc;
    struct smgrain g[SM_GRAINS];
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
    int16_t fx0[SM_VOICES];                 /* GUI: where each finger went down (tape) */
    uint8_t qi, div, pat, latch, atk, rel;  /* quantize (0 off, 1 1/4, 2 1/8, 3 1/16; in ARP: snap to slices), arp / grain rate, arp pattern, hold, attack / release choice */
    uint8_t gfree, a_last, sp_next, _q;     /* grain rate free (grains per second) instead of the grid; the spot played last; next spot to replace */
    uint16_t a_step, _q2;
    float scat, sph, dens;                  /* grain scatter 0..1; free-running phase in frames; free grain density per second */
    uint32_t tick, pf_t;
    uint32_t t_last, t_sum;                 /* cycle counter at the last run; cycles spent in this report */
    uint16_t t_n, t_peak, t_avg_shown, t_peak_shown;   /* load of samplr_run in per mille of the block period */                    /* blocks run; when the last load was asked for */
    struct smspot spot[SM_SPOTS];
    uint32_t rnd;
    int16_t kacc[4];                        /* knob counts not yet turned into a step */
    uint16_t filled;                        /* overview columns complete */
    uint8_t ofill[SM_COLS];
    float omn[SM_COLS], omx[SM_COLS];
    int8_t ov[2][SM_COLS];                  /* per column: lowest and highest sample, -127..127 */
    struct smvoice v[SM_VOICES];
    int32_t cut[SM_CUTS + 1];               /* slice i = frames cut[i] .. cut[i + 1] */
    int8_t drag[SM_VOICES];                 /* finger -> the slice point it moves (-1 none) */
    int8_t trans;                           /* transpose, semitones -48..48 */
    uint8_t ypit;                           /* bit per mode: finger height = pitch */
    uint8_t _t[2];
    float *tl, *tr;                         /* read buffers (SM_TMP floats each) */
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
void samplr_cycle(int what);                /* 0 quantize / snap / sync-free, 1 arp pattern, 2 latch, 3 height = pitch, 4 find transients */
void samplr_trans(int what);                /* transpose: +-1, +-12, 0 = back to 0 */
void samplr_knob(int knob, int counts);     /* knobs 1..3 per mode (the page handles knob 0 = volume) */
void samplr_touch(int kind, int id, int fx, int fy);   /* kind 0 down, 1 move, 2 up; fx, fy 0..1023 inside the waveform */
void samplr_name(char *out, int max);       /* the selected sample's name for the page */
void samplr_info(char *out);                /* the mode's settings as text for the top bar */
const char *samplr_pat_name(int p);
uint32_t samplr_sig(void);
