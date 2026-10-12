/*
 * MIDI clock monitor: measures, on the real unit, how well the stock firmware can follow an incoming MIDI clock.
 * The stock path (docs/midi-in-notes.md): USART6 receives one byte per interrupt into a ring, the audio block's render
 * drains the ring and hands 0xF8 to the sequencer, so every tick is seen at a block boundary (5.33 ms). USB packets
 * are copied in by an interrupt and parsed in the same render. This patch changes nothing in that path; it only stamps.
 *
 * Hooks (patches/clkmon.py):
 *   0x08044e48  the RxCplt callback's `bl 0x080446c4` (push one received byte): clkmon_rx stamps 0xF8 / FA / FB / FC with the
 *               cycle counter, then calls the stock function with the same arguments.
 *   0x08044f2c  the USB MIDI callback's `bl 0x080446da` (copy a packet buffer into the ring): clkmon_usb_rx scans the buffer
 *               for clock events, then calls the stock function.
 * Both run in interrupt context (USART6 has the highest priority), so they are a handful of instructions: no division, no
 * allocation, writes only to a fixed 104-byte block of the backup SRAM, and only while its clock and write access are on.
 * The audio block (clkmon_block, from t2m_audio) reads those stamps and keeps min / max / mean / deviation of
 *   ARR  the interval between two ticks as they arrived (the interrupt's view),
 *   STK  the same interval as the stock sequencer gets it (stamped at the audio block that takes the tick),
 *   LAT  how long a tick waits for that block.
 * The core clock comes from the SysTick reload register: the firmware sets the FreeRTOS tick to 1 kHz from its core clock
 * (vPortSetupTimerInterrupt, 0x080880e8: reload = SystemCoreClock / 1000 - 1), and the cycle counter runs at that clock.
 * The tempo is one beat's worth of ticks (24 intervals) over the time they took, so it follows a change within a beat.
 */
#include <stdint.h>

#include "clkmon.h"

#define DEMCR      (*(volatile uint32_t *)0xe000edfcu)
#define DWT_CTRL   (*(volatile uint32_t *)0xe0001000u)
#define DWT_CYCCNT (*(volatile uint32_t *)0xe0001004u)
#define DWT_LAR    (*(volatile uint32_t *)0xe0001fb0u)
#define SYST_RVR   (*(volatile uint32_t *)0xe000e014u)
#define RCC_AHB4ENR (*(volatile uint32_t *)0x580244e0u)
#define BKPRAMEN    (1u << 28)
#define PWR_CR1     (*(volatile uint32_t *)0x58024800u)
#define DBP         (1u << 8)

/* Backup SRAM 0x38800988..0x388009f0: after Trigger2MIDI's root (0x980), before fx2's state (0xa00). */
#define ISR ((volatile struct clk_isr *)0x38800988u)
_Static_assert(sizeof(struct clk_isr) <= 0x78, "the interrupt block must fit before 0x38800a00");

typedef void (*push_fn)(void *mgr, uint32_t byte, uint32_t port);
typedef void (*usb_fn)(void *mgr, const uint8_t *buf, uint32_t len, uint32_t z);
#define stock_push ((push_fn)0x080446c5u)
#define stock_usb  ((usb_fn)0x080446dbu)

#define GAP_CYCLES (1u << 27)           /* longer than this between two ticks = a stop / start, not an interval */

static inline int bkp_on(void)
{
    return (RCC_AHB4ENR & BKPRAMEN) && (PWR_CR1 & DBP);
}

static inline void stamp(volatile struct clk_isr_src *s, uint32_t t)
{
    s->ring[s->wr & (CLK_RING - 1)] = t;
    s->wr++;
    s->n_f8++;
}

/* USART6 receive callback: replaces the call that pushes the byte into the stock ring. */
void clkmon_rx(void *mgr, uint32_t byte, uint32_t port)
{
    uint32_t t0 = DWT_CYCCNT;
    int on = bkp_on() && port == 0;
    if (on && byte >= 0xf8) {
        volatile struct clk_isr_src *s = &ISR->s[0];
        if (byte == 0xf8)
            stamp(s, t0);
        else if (byte == 0xfa)
            s->n_fa++;
        else if (byte == 0xfb)
            s->n_fb++;
        else if (byte == 0xfc)
            s->n_fc++;
    }
    stock_push(mgr, byte, port);
    if (on) {
        uint32_t dt = DWT_CYCCNT - t0;
        if (dt > ISR->cost[0])
            ISR->cost[0] = dt;
    }
}

/* USB MIDI receive callback: replaces the call that copies the 512-byte packet buffer into the stock ring. USB-MIDI event
 * packets are 4 bytes: cable / code index, status, data 1, data 2; code index 0xF is a single byte (system real-time here).
 * The buffer is zeroed after every transfer, so a code index of 0 ends the packets. */
void clkmon_usb_rx(void *mgr, const uint8_t *buf, uint32_t len, uint32_t z)
{
    uint32_t t0 = DWT_CYCCNT;
    int on = bkp_on();
    if (on) {
        volatile struct clk_isr_src *s = &ISR->s[1];
        for (uint32_t i = 0; i + 4 <= len && (buf[i] & 0x0f); i += 4) {
            if ((buf[i] & 0x0f) != 0x0f)
                continue;
            uint8_t st = buf[i + 1];
            if (st == 0xf8)
                stamp(s, t0);
            else if (st == 0xfa)
                s->n_fa++;
            else if (st == 0xfb)
                s->n_fb++;
            else if (st == 0xfc)
                s->n_fc++;
        }
    }
    stock_usb(mgr, buf, len, z);
    if (on) {
        uint32_t dt = DWT_CYCCNT - t0;
        if (dt > ISR->cost[1])
            ISR->cost[1] = dt;
    }
}

void clkmon_boot(struct clkmon *c)
{
    DEMCR |= 1u << 24;                  /* TRCENA */
    DWT_LAR = 0xc5acce55u;
    DWT_CTRL |= 1u;                     /* CYCCNTENA (cpu.c does the same) */
    volatile uint32_t *p = (volatile uint32_t *)ISR;
    for (unsigned i = 0; i < sizeof(struct clk_isr) / 4; i++)
        p[i] = 0;
    uint32_t *q = (uint32_t *)c;
    for (unsigned i = 0; i < sizeof *c / 4; i++)
        q[i] = 0;
}

static float fsqrt(float x)
{
    float r;
    __asm__("vsqrt.f32 %0, %1" : "=w"(r) : "w"(x));
    return r;
}

static void add_interval(uint32_t *n, uint32_t *mn, uint32_t *mx, float *mean, float *m2, uint32_t d)
{
    float x = (float)d;
    if (*n == 0 || d < *mn)
        *mn = d;
    if (*n == 0 || d > *mx)
        *mx = d;
    (*n)++;
    float dl = x - *mean;
    *mean += dl / (float)*n;
    *m2 += dl * (x - *mean);
}

static void clear_src(struct clk_rd *r, const volatile struct clk_isr_src *s)
{
    uint32_t *q = (uint32_t *)r;
    for (unsigned i = 0; i < sizeof *r / 4; i++)
        q[i] = 0;
    r->rd = s->wr;
    r->base_f8 = s->n_f8;
    r->base_fa = s->n_fa;
    r->base_fb = s->n_fb;
    r->base_fc = s->n_fc;
}

void clkmon_block(struct clkmon *c)
{
    uint32_t now = DWT_CYCCNT;
    if (!bkp_on())
        return;
    if (c->reset) {
        c->reset = 0;
        c->have_blk = c->blk_min = c->blk_max = 0;
        for (int k = 0; k < CLK_SRC; k++)
            clear_src(&c->r[k], &ISR->s[k]);
    }
    uint32_t rvr = SYST_RVR & 0xffffffu;
    c->mhz = rvr >= 49999u && rvr <= 599999u ? (float)(rvr + 1u) / 1000.f : 0.f;     /* 50 .. 600 MHz */
    if (c->have_blk) {
        uint32_t d = now - c->blk_prev;
        if (!c->blk_max || d < c->blk_min)
            c->blk_min = d;
        if (d > c->blk_max)
            c->blk_max = d;
    }
    c->blk_prev = now;
    c->have_blk = 1;
    for (int k = 0; k < CLK_SRC; k++) {
        struct clk_rd *r = &c->r[k];
        volatile struct clk_isr_src *s = &ISR->s[k];
        uint32_t wr = s->wr;
        if (wr - r->rd > CLK_RING) {
            r->over += wr - r->rd - CLK_RING;
            r->rd = wr - CLK_RING;
        }
        if (r->span && c->mhz > 0.f && now - r->last_arr > (uint32_t)(c->mhz * 1e6f))
            r->span = 0;                 /* no tick for a second: no tempo */
        while (r->rd != wr) {
            uint32_t t = s->ring[r->rd & (CLK_RING - 1)];
            r->rd++;
            uint32_t lat = now - t;
            r->lat_sum += (float)lat;
            r->n_lat++;
            if (lat > r->lat_max)
                r->lat_max = lat;
            if (r->have_arr && t - r->last_arr >= GAP_CYCLES) {        /* a stop / start: the tempo window starts again */
                r->win_n = 0;
                r->span = 0;
            }
            if (r->have_arr && t - r->last_arr < GAP_CYCLES)
                add_interval(&r->n_arr, &r->arr_min, &r->arr_max, &r->arr_mean, &r->arr_m2, t - r->last_arr);
            r->win[r->win_n % 25u] = t;
            uint32_t k = r->win_n < 24u ? r->win_n : 24u;              /* intervals in the window so far */
            if (k >= 4u) {
                r->span = t - r->win[(r->win_n - k) % 25u];
                r->span_k = k;
            }
            r->win_n++;
            r->last_arr = t;
            r->have_arr = 1;
            if (r->have_stk && now - r->last_stk < GAP_CYCLES)
                add_interval(&r->n_stk, &r->stk_min, &r->stk_max, &r->stk_mean, &r->stk_m2, now - r->last_stk);
            r->last_stk = now;
            r->have_stk = 1;
        }
    }
}

float clkmon_mhz(const struct clkmon *c)
{
    return c->mhz;
}

float clkmon_blk_ms(const struct clkmon *c, int max)
{
    float mhz = c->mhz > 50.f ? c->mhz : 400.f;
    return (float)(max ? c->blk_max : c->blk_min) / (mhz * 1000.f);
}

void clkmon_stats(const struct clkmon *c, int src, struct clk_stats *o)
{
    const struct clk_rd *r = &c->r[src];
    const volatile struct clk_isr_src *s = &ISR->s[src];
    float mhz = c->mhz > 50.f ? c->mhz : 400.f;
    float us = 1.f / mhz;
    o->f8 = s->n_f8 - r->base_f8;
    o->fa = (uint16_t)(s->n_fa - r->base_fa);
    o->fb = (uint16_t)(s->n_fb - r->base_fb);
    o->fc = (uint16_t)(s->n_fc - r->base_fc);
    o->over = r->over;
    o->n = r->n_arr;
    o->bpm = r->span ? 60e6f * mhz * (float)r->span_k / (24.f * (float)r->span) : 0.f;
    o->arr_min = r->n_arr ? (float)r->arr_min * us : 0.f;
    o->arr_max = r->n_arr ? (float)r->arr_max * us : 0.f;
    o->arr_sd = r->n_arr > 1 ? fsqrt(r->arr_m2 / (float)(r->n_arr - 1)) * us : 0.f;
    o->stk_min = r->n_stk ? (float)r->stk_min * us : 0.f;
    o->stk_max = r->n_stk ? (float)r->stk_max * us : 0.f;
    o->stk_sd = r->n_stk > 1 ? fsqrt(r->stk_m2 / (float)(r->n_stk - 1)) * us : 0.f;
    o->lat_avg = r->n_lat ? r->lat_sum / (float)r->n_lat * us : 0.f;
    o->lat_max = (float)r->lat_max * us;
    o->cost = ISR->cost[src];
}

uint32_t clkmon_sig(const struct clkmon *c)
{
    uint32_t h = 2166136261u;
    for (int k = 0; k < CLK_SRC; k++) {
        h = (h ^ ISR->s[k].n_f8) * 16777619u;
        h = (h ^ ISR->s[k].n_fa ^ ((uint32_t)ISR->s[k].n_fb << 8) ^ ((uint32_t)ISR->s[k].n_fc << 16)) * 16777619u;
        h = (h ^ c->r[k].n_arr ^ c->r[k].over) * 16777619u;
    }
    return (h ^ (uint32_t)(c->mhz * 10.f)) * 16777619u;
}
