/* MIDI clock monitor (src/clkmon.c): when do 0xF8 clock ticks really arrive, and when does the stock firmware see them. */
#pragma once
#include <stdint.h>

#define CLK_SRC  2                   /* 0 = TRS jack (USART6), 1 = USB */
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
    uint32_t win[25], win_n;         /* the last 25 arrival stamps: 24 ticks = one beat, for the tempo */
    uint32_t span, span_k;           /* cycles the last span_k (4..24) intervals took; span 0 = no clock right now */
};
struct clkmon {
    struct clk_rd r[CLK_SRC];
    uint32_t blk_prev, have_blk;
    uint32_t blk_min, blk_max;       /* cycles between two audio blocks, since the last reset */
    float mhz;                       /* core clock from the SysTick reload (the firmware's 1 kHz tick); 0 = not plausible */
    uint32_t reset;                  /* set by the page */
};

/* Everything in microseconds. */
struct clk_stats {
    uint32_t f8, fa, fb, fc, over, n;
    float bpm;
    float arr_min, arr_max, arr_sd;
    float stk_min, stk_max, stk_sd;
    float lat_avg, lat_max;
    uint32_t cost;                   /* cycles */
};

void clkmon_boot(struct clkmon *c);                  /* from t2m_boot, with the backup SRAM on */
void clkmon_block(struct clkmon *c);                 /* from t2m_audio, once per audio block */
void clkmon_stats(const struct clkmon *c, int src, struct clk_stats *o);
float clkmon_mhz(const struct clkmon *c);            /* 0 until the first audio block */
float clkmon_blk_ms(const struct clkmon *c, int max); /* the shortest (0) / longest (1) gap between two audio blocks, ms */
uint32_t clkmon_sig(const struct clkmon *c);
