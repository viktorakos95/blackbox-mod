/* MIDI clock monitor (src/clkmon.c): when do 0xF8 clock ticks really arrive, and when does the stock firmware see them. */
#pragma once
#include <stdint.h>

#define CLK_SRC  2                   /* 0 = TRS jack (USART6), 1 = USB */
#define CLK_WIN  48                  /* intervals the tempo fit looks at (2 beats) */
#define CLK_XPW  32                  /* the sequencer-position fit looks at this many block calls */
#define CLK_RING 8                   /* arrival stamps the interrupt can hold between two audio blocks */

/* The interrupt side lives in the backup SRAM (no allocation, nothing the interrupt could trip over before boot). */
struct clk_isr_src {
    uint32_t wr;                     /* stamps written, ever */
    uint32_t n_f8;
    uint16_t n_fa, n_fb;             /* start, continue */
    uint16_t n_fc, _r;               /* stop */
    uint32_t ring[CLK_RING];         /* CYCCNT at arrival */
};
struct clk_isr {
    struct clk_isr_src s[CLK_SRC];
    uint32_t cost[CLK_SRC];          /* worst cycles spent inside the hook (stock work included), since boot */
};

/* The audio-block side lives in the Trigger2MIDI memory (src/t2m_bb.h). Only 4-byte fields: the tests build it on the host. */
struct clk_rd {
    uint32_t rd;                     /* stamps consumed */
    uint32_t last_arr, last_stk;
    uint32_t have_arr, have_stk;
    uint32_t n_arr, arr_min, arr_max;
    float arr_mean, arr_m2;          /* Welford over the arrival-to-arrival cycles */
    uint32_t n_stk, stk_min, stk_max;
    float stk_mean, stk_m2;          /* the same, as the block-quantised stock path sees it */
    uint32_t n_lat, lat_max;
    float lat_sum;                   /* arrival -> the block that takes it */
    uint32_t over;                   /* stamps lost to a full ring */
    uint32_t base_f8, base_fa, base_fb, base_fc;     /* counters at the last reset */
    uint32_t win[CLK_WIN + 1], win_n; /* the last arrival stamps, for the tempo: a straight-line fit through them */
    float bpm;                       /* the tempo shown (0 = no clock right now); moves only when the fit moves by more than the noise */
};
/* The stock sequencer's musical time (src/seqfix.c hands it over for every sequence the player runs, once per audio block while
 * the transport plays; the tick size is what the hardware readout is for)
 * against the audio's own time (frames processed), the way a plugin host's position would be judged. */
struct clk_xp {
    uint32_t have;                   /* a previous call to compare with */
    uint32_t calls;
    uint32_t last_s, last_t;         /* frames and ticks (low word) at the previous call */
    int32_t dt_min, dt_max;          /* ticks per call, between calls one block apart */
    uint32_t dt_n;
    uint32_t w_n, w_head;            /* the window: this many (frames, ticks) points, newest at w_head - 1 */
    uint32_t wx[CLK_XPW], wt[CLK_XPW];
    float slope;                     /* ticks per frame from the last fit */
    uint32_t r_n;
    float r_mean, r_m2, r_min, r_max; /* the newest point against the fit of the earlier ones, in microseconds */
    uint32_t restarts;               /* the window started again: a stop, a loop jump, a gap in the calls */
    uint32_t seq_id;                 /* the sequence followed (the first one seen since the last reset) */
    uint32_t stalls;                 /* blocks where its time did not move although the audio did */
    uint32_t slips;                  /* points more than 1 ms off the line */
    uint32_t misses;                 /* blocks with no call for it in between two that had one */
    uint32_t blk_s, blk_calls, seqs_max;  /* calls in the current block, the most seen in one block */
    uint32_t blk_ref, blk_have_ref, blk_other, blk_have_other;
    uint32_t spread;                 /* the largest difference between two sequences' times in one block, ticks */
    uint32_t pend_have, pend_s, pend_t; /* the followed sequence's last time in the current block, fed in when the next block starts */
};
struct clkmon {
    struct clk_rd r[CLK_SRC];
    struct clk_xp xp;
    uint32_t samples;                /* frames of audio seen by clkmon_block (this block's included) */
    uint32_t last_frames;            /* the previous block's size */
    uint32_t fs_samples, fs_lo, fs_hi; /* frames and cycles (64 bit) between audio blocks since the last reset: the sample rate */
    uint32_t blk_prev, have_blk;
    uint32_t blk_min, blk_max;       /* cycles between two audio blocks, since the last reset */
    float mhz;                       /* core clock from the SysTick reload (the firmware's 1 kHz tick); 0 = not plausible */
    uint32_t reset;                  /* set by the page */
};

/* Everything in microseconds. */
struct clk_xp_stats {
    uint32_t calls, restarts;
    int32_t dt_min, dt_max;          /* ticks per block */
    float tps;                       /* ticks per second from the fit; 0 = no fit yet */
    float res_min, res_max, res_sd;  /* microseconds */
    float fs;                        /* audio sample rate measured against the core clock (Hz); 0 = not yet */
    uint32_t stalls, slips, misses, seqs_max, spread;
};

struct clk_stats {
    uint32_t f8, fa, fb, fc, over, n;
    float bpm;
    float arr_min, arr_max, arr_sd;
    float stk_min, stk_max, stk_sd;
    float lat_avg, lat_max;
    uint32_t cost;                   /* cycles */
};

void clkmon_boot(struct clkmon *c);                  /* from t2m_boot, with the backup SRAM on */
void clkmon_block(struct clkmon *c, int frames);      /* from t2m_audio, once per audio block of `frames` frames */
void clkmon_seq(struct clkmon *c, const void *seq, uint32_t lo, int32_t hi);   /* a sequence's musical time, from seq_play (once per sequence per block) */
void clkmon_xp_stats(const struct clkmon *c, struct clk_xp_stats *o);
void clkmon_stats(const struct clkmon *c, int src, struct clk_stats *o);
float clkmon_mhz(const struct clkmon *c);            /* 0 until the first audio block */
float clkmon_blk_ms(const struct clkmon *c, int max); /* the shortest (0) / longest (1) gap between two audio blocks, ms */
uint32_t clkmon_sig(const struct clkmon *c);
