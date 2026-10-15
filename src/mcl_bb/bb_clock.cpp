/*
 * MIDI clock source for MCL (milestones M4, M5): a hardware timer, independent of the 128-frame audio block (2.67 ms) and of the stock output queue.
 *
 * TIM6 (a basic 16-bit timer, vector 54; found unused in the stock firmware: no handler, no references) counts at about 1 MHz and interrupts on every
 * update event. The interrupt sets the length of the NEXT interval from a 32.32 fixed-point accumulator (period = 60 * rate / (192 * bpm) timer counts),
 * so the whole-count rounding error is carried to the following tick instead of lost: over any run the tick times are the ideal ones to within one
 * timer count, and an interrupt that is served late does not move the next one (the counter reloads by itself). Every 8th tick (24 PPQN) a 0xF8 goes to
 * MCL's TX ring as a real-time byte, which leaves at the next byte boundary even in the middle of a sysex dump. Priority 1: above the audio DMA
 * (5..9) and FreeRTOS' critical sections (>= 5), below the USART6 interrupt (0). No FreeRTOS call, no stock queue.
 *
 * The timer's input clock is not assumed: bb_clock_init() counts it against the DWT cycle counter (core clock from the SysTick reload, the firmware's 1 kHz tick).
 *
 * MIDI-in sync (M5). The receive tap (priority 0, tens of cycles) stamps every F8 with the cycle counter into a small ring and passes FA / FB / FC on as
 * commands; ALL loop state is touched only by the timer interrupt, which drains the ring at the start of each tick, so nothing is shared with a higher
 * priority writer. The loop is a phase-locked loop on the 24-PPQN clock: the phase detector is the time of the input clock relative to our own last
 * 24-PPQN tick (wrapped to +-half a period), the filter is proportional (1/4 of the error moves the next tick) plus integral (1/64 of the error changes the
 * period), a critically damped second-order loop with a natural frequency of about 1 Hz at 120 BPM (it follows a tempo ramp of 2 BPM per second with a lag of about half a millisecond). So the input's own jitter (KeyStep ~35 us, Machinedrum ~70 us,
 * a 1 ms software clock) is smoothed instead of copied, tempo changes are followed within about a second, and the F8 we send leaves at the instant the input's did,
 * predicted from the period. Acquisition: two clocks give the period and start the timer; a phase error over an eighth of a period is taken as a slip and
 * corrected at once; no clock for four periods = input lost, the tempo is held ("flywheel") for 2 s, then the clock stops and the loop listens again.
 */
#include "bb_clock.h"
#include "bb_midi.h"

#define TIM6        0x40001000u
#define T_CR1       (*(volatile uint32_t *)(TIM6 + 0x00))
#define T_DIER      (*(volatile uint32_t *)(TIM6 + 0x0c))
#define T_SR        (*(volatile uint32_t *)(TIM6 + 0x10))
#define T_EGR       (*(volatile uint32_t *)(TIM6 + 0x14))
#define T_CNT       (*(volatile uint32_t *)(TIM6 + 0x24))
#define T_PSC       (*(volatile uint32_t *)(TIM6 + 0x28))
#define T_ARR       (*(volatile uint32_t *)(TIM6 + 0x2c))
#define RCC_APB1LENR (*(volatile uint32_t *)0x580244e8u)        /* bit 4: TIM6 */
#define NVIC_ISER1  (*(volatile uint32_t *)0xe000e104u)         /* IRQ 32..63 */
#define NVIC_IPR54  (*(volatile uint8_t *)(0xe000e400u + 54))
#define SYST_CSR    (*(volatile uint32_t *)0xe000e010u)
#define SYST_RVR    (*(volatile uint32_t *)0xe000e014u)
#define DWT_CYCCNT  (*(volatile uint32_t *)0xe0001004u)
#define HOOK __attribute__((section(".hook"), used))

#define READY_MAGIC 0x434c4b31u                                 /* "CLK1" */
static volatile uint32_t ready;
static volatile uint32_t running;                               /* internal mode: the clock is being sent */
static volatile uint32_t sub;                                   /* 0..7: 192-PPQN ticks since the last 24-PPQN clock */
static volatile uint32_t ticks;
static volatile uint32_t isr_calls;                             /* every entry of the timer interrupt, ready or not (diagnostics) */
static volatile uint32_t pending_start;                         /* send FA at the next tick */
static volatile uint32_t pending_zero;                          /* a received FA: the 192-PPQN tick counter restarts at the next 24-PPQN clock */
static uint32_t timer_hz;                                       /* counts per second after the prescaler */
static uint32_t core_cps;                                       /* core cycles per second */
static uint32_t cpc_q16;                                        /* core cycles per timer count, Q16 */
static volatile uint32_t period_hi, period_lo;                  /* 32.32 counts per 192-PPQN tick */
static volatile uint32_t acc;                                   /* fractional part carried between ticks */
static void (*volatile tick_hook)(uint32_t);

/* --- MIDI-in sync state (written by the timer interrupt only, except the two rings' write ends) */
enum { SY_INTERNAL = 0, SY_LISTEN = 1, SY_ONE = 2, SY_LOCKED = 3, SY_LOST = 4 };
#define IN_N 16u
static volatile uint32_t in_t[IN_N];                            /* arrival times of F8, cycle counter */
static volatile uint32_t in_wr, in_rd;
static volatile uint8_t in_cmd[IN_N];                           /* FA / FB / FC, in order */
static volatile uint32_t cmd_wr, cmd_rd;
static volatile uint32_t sync_state;
static uint64_t P_q16;                                          /* core cycles per 24-PPQN clock, Q16 */
static uint32_t t_sub0;                                         /* cycle counter at our last 24-PPQN tick */
static uint32_t t_prev_in;                                      /* and of the previous input clock */
static uint32_t t_last_in;                                      /* ... of the last input clock (loss detection) */
static int32_t pend_adj;                                        /* timer counts to add to the next interval (phase correction) */
static uint32_t lost_ticks;                                     /* ticks spent flywheeling */
static uint32_t e_max_us, e_last_us, e_win_max, e_win_n, n_clocks;
static uint32_t acq_now;                                         /* acquisition happened in this interrupt: it already programmed the first interval */

/* Follow the Blackbox's own transport (M5b): seqfix.c stamps (cycle counter, sequencer time at 960 a beat) at every call of the stock note player; the interrupt fits a
 * straight line through the last SQ_N stamps of one sequence and turns it into "virtual" input clocks for the same PLL: the instants the fitted line passes every 40th tick. */
#define SQ_STAMP    ((volatile uint32_t *)0x38800940u)       /* {version, sequence, cycles, now_lo, now_hi} */
#define SQ_N        128                                         /* 340 ms of stamps at 120 BPM: the slope of the line is steadier the longer the window */
#define SQ_MIN      8                                           /* stamps before the line is trusted */
static volatile uint32_t src_seq;                               /* the input is the stamps, not the jack */
static uint32_t sq_ver, sq_prim, sq_prim_t;                    /* last version seen, the followed sequence, its last stamp's cycles */
static uint32_t sq_cyc[SQ_N];
static int64_t sq_now[SQ_N];
static uint32_t sq_n, sq_wr;                                    /* stamps in the window, next slot */
static int64_t sq_next_k;                                       /* next 24-PPQN clock (sequencer time / 40) to turn into a virtual input clock */
static uint32_t sq_running, sq_announce, sq_from_zero, sq_resync;
static uint32_t sq_restarts, sq_accepted;
static int32_t sq_offset_cyc;                                   /* shifts the virtual clocks later (audio output delay), core cycles */
#define POLL_COUNTS   1000u                                     /* listening: look at the stamp ring every ~1 ms */
#define KP_SHIFT      2                                         /* proportional gain 1/4 */
#define KI_SHIFT      6                                         /* integral gain 1/64: critically damped, natural frequency about 1 Hz at 120 BPM */
#define FLYWHEEL_S    2u

extern "C" {

HOOK void bb_clock_down(void)
{
    ready = 0;
    running = 0;
    isr_calls = 0;
    ticks = 0;
    sync_state = SY_INTERNAL;
}       /* TIM6 itself is not touched: its interrupt returns at once while ready is off */

static uint32_t core_hz(void)
{
    uint32_t hz = (SYST_RVR + 1u) * 1000u;                       /* the FreeRTOS tick is 1 kHz */
    return (SYST_CSR & 4u) ? hz : hz * 8u;                       /* CLKSOURCE: 1 = core clock, 0 = core / 8 */
}

HOOK int bb_clock_init(void)
{
    ready = 0;
    running = 0;
    sync_state = SY_INTERNAL;
    RCC_APB1LENR |= 0x10u;
    (void)RCC_APB1LENR;
    T_CR1 = 0;
    T_DIER = 0;
    T_PSC = 0;
    T_ARR = 0xffff;
    T_EGR = 1;                                                   /* load PSC / ARR */
    T_SR = 0;
    T_CR1 = 1;                                                   /* free running, to measure it */
    uint64_t counts = 0, cycles = 0;
    for (int i = 0; i < 16; i++) {
        uint32_t c0 = DWT_CYCCNT, t0 = T_CNT;
        uint32_t c1;
        do
            c1 = DWT_CYCCNT;
        while (c1 - c0 < 60000u);                                // ~150 us: fewer counts than the 16-bit counter holds if the timer is not faster than the core
        uint32_t t1 = T_CNT;
        counts += (t1 - t0) & 0xffffu;
        cycles += c1 - c0;
    }
    T_CR1 = 0;
    if (!counts || !cycles)
        return 0;
    core_cps = core_hz();
    uint64_t hz = (uint64_t)core_cps * counts / cycles;          /* timer clock at PSC = 0 */
    uint32_t psc = (uint32_t)((hz + 500000u) / 1000000u);        /* prescaler divisor for about 1 MHz */
    if (psc < 1)
        psc = 1;
    T_PSC = psc - 1u;
    timer_hz = (uint32_t)(hz / psc);
    cpc_q16 = (uint32_t)(((uint64_t)core_cps << 16) / timer_hz); /* cycles per count */
    T_EGR = 1;
    T_SR = 0;
    NVIC_IPR54 = 0x10;                                           /* priority 1 (4 bits used, top-aligned) */
    return timer_hz > 100000u && timer_hz < 10000000u;
}

HOOK uint32_t bb_clock_timer_hz(void) { return timer_hz; }
HOOK uint32_t bb_clock_prescaler(void) { return T_PSC + 1u; }

static void set_period_counts(uint64_t counts_q32)
{
    period_hi = (uint32_t)(counts_q32 >> 32);
    period_lo = (uint32_t)counts_q32;
}

HOOK void bb_clock_tempo(uint32_t bpm_x10)
{
    if (bpm_x10 < 200u)
        bpm_x10 = 200u;
    if (bpm_x10 > 3000u)
        bpm_x10 = 3000u;
    /* counts per 192-PPQN tick = 60 * hz / (192 * bpm) = 600 * hz / (192 * bpm_x10), as 32.32 */
    set_period_counts(((uint64_t)600u * timer_hz << 32) / (192ull * bpm_x10));
}

/* the 24-PPQN period in core cycles (Q16) -> counts per 192-PPQN tick, 32.32:  P / 8 / cpc  (the shift by 24 keeps it inside 64 bits up to 20 BPM) */
static void period_from_cycles(void)
{
    uint64_t c_q16 = P_q16 >> 3;
    set_period_counts(((c_q16 << 24) / cpc_q16) << 8);
}

HOOK void bb_clock_start(uint32_t bpm_x10, int send_start)
{
    if (!timer_hz)
        return;
    src_seq = 0;
    T_CR1 = 0;
    T_DIER = 0;
    sync_state = SY_INTERNAL;
    bb_clock_tempo(bpm_x10);
    sub = 0;
    ticks = 0;
    acc = period_lo;
    pending_start = send_start ? 1u : 0u;
    T_ARR = (period_hi ? period_hi : 1u) - 1u;
    T_EGR = 1;
    T_SR = 0;
    ready = READY_MAGIC;
    running = 1;
    NVIC_ISER1 = 1u << (54 - 32);
    T_DIER = 1;
    T_CR1 = 1;
}

static void sync_begin(void);
static void sq_reset(void);
HOOK void bb_clock_sync_midi(void)
{
    if (!timer_hz)
        return;
    src_seq = 0;
    sync_begin();
}

/* follow the Blackbox's own sequencer: internal tempo, TRS MIDI or the analog jack, whatever drives it (seqfix.c's stamps) */
HOOK void bb_clock_sync_seq(void)
{
    if (!timer_hz)
        return;
    T_CR1 = 0;
    T_DIER = 0;
    src_seq = 0;
    sq_reset();
    sq_ver = SQ_STAMP[0];
    sq_prim = 0;
    sq_running = sq_announce = sq_resync = 0;
    sq_restarts = sq_accepted = 0;
    sq_next_k = 0;
    sync_begin();
    src_seq = 1;
}

HOOK void bb_clock_seq_offset_us(int32_t us) { sq_offset_cyc = (int32_t)((int64_t)us * (int64_t)core_cps / 1000000); }

HOOK uint32_t bb_clock_seq_stat(int which)
{
    switch (which) {
    case 0: return sq_accepted;
    case 1: return sq_restarts;
    case 2: return sq_running;
    default: return (uint32_t)sq_now[(sq_wr - 1u) & (SQ_N - 1)];
    }
}

static void sync_begin(void)
{
    T_CR1 = 0;
    T_DIER = 0;
    running = 0;
    in_wr = in_rd = cmd_wr = cmd_rd = 0;
    sub = 0;
    ticks = 0;
    acc = 0;
    pend_adj = 0;
    pending_start = pending_zero = 0;
    e_max_us = e_last_us = e_win_max = e_win_n = n_clocks = 0;
    lost_ticks = 0;
    sync_state = SY_LISTEN;
    T_ARR = POLL_COUNTS - 1u;                                    /* listen: the interrupt looks at the stamp ring every ~1 ms */
    T_EGR = 1;
    T_SR = 0;
    ready = READY_MAGIC;
    NVIC_ISER1 = 1u << (54 - 32);
    T_DIER = 1;
    T_CR1 = 1;
}

HOOK void bb_clock_stop(int send_stop)
{
    src_seq = 0;
    T_DIER = 0;
    T_CR1 = 0;
    running = 0;
    sync_state = SY_INTERNAL;
    if (send_stop)
        for (uint32_t spin = 0; spin < 200000u && !bb_midi_tx_rt(0xfc); spin++) {}      /* the real-time FIFO drains at 31250 baud: wait for a slot (GUI task, ~ms) */
}

HOOK int bb_clock_running(void) { return running || sync_state == SY_LOCKED || sync_state == SY_LOST; }
HOOK uint32_t bb_clock_isr_calls(void) { return isr_calls; }
HOOK uint32_t bb_clock_ticks(void) { return ticks; }
HOOK void bb_clock_set_hook(void (*tick)(uint32_t)) { tick_hook = tick; }
HOOK int bb_clock_sync_state(void) { return (int)sync_state; }

HOOK uint32_t bb_clock_bpm_x10(void)
{
    if (sync_state == SY_INTERNAL || !P_q16)
        return 0;
    return (uint32_t)((25ull * core_cps << 16) / P_q16);             /* 600 * core / (24 * P) */
}

HOOK uint32_t bb_clock_err_us(int which)
{
    switch (which) {
    case 0: return e_max_us;
    case 1: return e_last_us;
    default: return n_clocks;
    }
}

/* receive tap, interrupt context (priority 0): the arrival time of a real-time byte */
HOOK void bb_clock_in_rt(uint8_t byte, uint32_t cycles)
{
    if (ready != READY_MAGIC || sync_state == SY_INTERNAL || src_seq)
        return;
    if (byte == 0xf8) {
        uint32_t w = in_wr;
        if (w - in_rd < IN_N) {
            in_t[w & (IN_N - 1)] = cycles;
            in_wr = w + 1;
        }
    } else if (byte >= 0xfa && byte <= 0xfc) {
        uint32_t w = cmd_wr;
        if (w - cmd_rd < IN_N) {
            in_cmd[w & (IN_N - 1)] = byte;
            cmd_wr = w + 1;
        }
    }
}

static uint32_t abs32(int32_t v) { return v < 0 ? (uint32_t)-v : (uint32_t)v; }

/* One input clock, arrival time t (cycle counter), seen at `now` by the timer interrupt. Updates the period, the phase correction, the status. */
static void pll_clock(uint32_t t, uint32_t now)
{
    t_last_in = t;
    n_clocks++;
    if (sync_state == SY_LISTEN) {
        t_prev_in = t;
        sync_state = SY_ONE;
        return;
    }
    if (sync_state == SY_ONE) {
        uint32_t pc = t - t_prev_in;
        t_prev_in = t;
        uint64_t pmin = (uint64_t)core_cps * 60u / (24u * 300u), pmax = (uint64_t)core_cps * 60u / (24u * 20u);        /* 300 and 20 BPM */
        if (pc < pmin || pc > pmax)
            return;                                              /* not a clock interval: wait for the next */
        P_q16 = (uint64_t)pc << 16;
        period_from_cycles();
        /* the next 24-PPQN tick should fall where the next input clock will: t + P. The interrupt is running at `now`, the timer was just reloaded. */
        int32_t wait = (int32_t)(t + pc - now);
        if (wait < (int32_t)(cpc_q16 >> 16) * 8)
            wait += (int32_t)pc;                                 /* too close: take the one after */
        uint32_t counts = (uint32_t)(((uint64_t)wait << 16) / cpc_q16);
        T_ARR = (counts > 0x10000u ? 0x10000u : counts ? counts : 1u) - 1u;
        acc = 0;
        sub = 0;
        t_sub0 = t + pc;
        lost_ticks = 0;
        sync_state = SY_LOCKED;
        acq_now = 1;
        e_max_us = e_win_max = e_win_n = 0;
        return;
    }
    /* locked: phase error against our last 24-PPQN tick, wrapped to +-half a period */
    int32_t pc = (int32_t)(P_q16 >> 16);
    int32_t e = (int32_t)(t - t_sub0);
    while (e > pc / 2)
        e -= pc;
    while (e < -pc / 2)
        e += pc;
    uint32_t iv = t - t_prev_in;
    t_prev_in = t;
    lost_ticks = 0;
    if (sync_state == SY_LOST)
        sync_state = SY_LOCKED;
    uint32_t e_us = (uint32_t)((uint64_t)abs32(e) * 1000000u / core_cps);
    e_last_us = e_us;
    if (e_us > e_win_max)
        e_win_max = e_us;
    if (++e_win_n >= 48u) {                                      /* about a second of clocks at 120 BPM */
        e_max_us = e_win_max;
        e_win_max = e_win_n = 0;
    }
    if (abs32(e) > (uint32_t)pc / 8u) {                          /* a slip (start, locate, tempo jump): take the phase at once, restart the tempo from the last interval */
        pend_adj += (int32_t)(((int64_t)e << 16) / (int32_t)cpc_q16);
        uint64_t lo = (uint64_t)core_cps * 60u / (24u * 300u), hi = (uint64_t)core_cps * 60u / (24u * 20u);
        if (iv >= lo && iv <= hi) {                               /* the last interval is a clock interval: start the tempo from it */
            P_q16 = (uint64_t)iv << 16;
            period_from_cycles();
        }
        return;
    }
    pend_adj += (int32_t)(((int64_t)(e >> KP_SHIFT) << 16) / (int32_t)cpc_q16);      /* proportional: move the next tick */
    if (!src_seq)
        P_q16 += (int64_t)((int64_t)e << 16) >> KI_SHIFT;       /* integral: change the period (not when following the sequencer: its fit gives the period) */
    uint64_t pmin = (uint64_t)core_cps * 60u / (24u * 300u), pmax = (uint64_t)core_cps * 60u / (24u * 20u);
    if ((P_q16 >> 16) < pmin)
        P_q16 = pmin << 16;
    if ((P_q16 >> 16) > pmax)
        P_q16 = pmax << 16;
    period_from_cycles();
}

static void sq_reset(void)
{
    sq_n = sq_wr = 0;
}

static void sq_push_clock(uint32_t t)
{
    uint32_t w = in_wr;
    if (w - in_rd < IN_N) {
        in_t[w & (IN_N - 1)] = t;
        in_wr = w + 1;
    }
}

/* The fit of the window: sequencer time (ticks) against the cycle counter. x in units of 256 cycles since the oldest stamp, y in ticks since the oldest; the line
 * is y = Sy/n + (Sxy/Sxx) (x - xm) with Sxy taken against x - xm, so no division loses a fraction of a tick. Returns 0 when there is no usable line. */
struct sq_fit {
    int64_t sy, sxy, sxx;
    int32_t xm;
    uint32_t n, cyc_o;
    int64_t now_o;
};

static int sq_line(sq_fit *f)
{
    uint32_t n = sq_n, o = (sq_wr - n) & (SQ_N - 1);
    if (n < SQ_MIN)
        return 0;
    f->n = n;
    f->cyc_o = sq_cyc[o];
    f->now_o = sq_now[o];
    int64_t sx = 0, sy = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = (o + i) & (SQ_N - 1);
        sx += (int32_t)((sq_cyc[k] - f->cyc_o) >> 8);
        sy += (int32_t)(sq_now[k] - f->now_o);
    }
    f->xm = (int32_t)(sx / (int64_t)n);
    f->sy = sy;
    int64_t sxx = 0, sxy = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = (o + i) & (SQ_N - 1);
        int32_t dx = (int32_t)((sq_cyc[k] - f->cyc_o) >> 8) - f->xm;
        sxx += (int64_t)dx * dx;
        sxy += (int64_t)dx * (int32_t)(sq_now[k] - f->now_o);
    }
    f->sxx = sxx;
    f->sxy = sxy;
    if (sxx <= 0 || sxy <= 0)
        return 0;
    /* 20 .. 300 BPM = 320 .. 4800 ticks a second = sxy / sxx * core_cps / 256 */
    int64_t q = sxy * (int64_t)core_cps / 256;
    return q > sxx * 320 && q < sxx * 4800;
}

/* The instant (cycle counter) the fitted line reaches sequencer time `target` (ticks, absolute). */
static uint32_t sq_when(const sq_fit *f, int64_t target)
{
    int64_t num = (target - f->now_o) * (int64_t)f->n - f->sy;           /* ticks * n above the line's mean */
    int64_t dx = num * f->sxx / ((int64_t)f->n * f->sxy);                /* x - xm */
    return f->cyc_o + (uint32_t)(((int64_t)f->xm + dx) << 8);
}

static void sq_stopped(void)
{
    sq_reset();
    if (!sq_running)
        return;
    sq_running = 0;
    sq_announce = 0;
    pending_start = pending_zero = 0;
    bb_midi_tx_rt(0xfc);
    in_rd = in_wr;
    sync_state = SY_LISTEN;                                      /* the PLL starts over at the next start */
    T_ARR = POLL_COUNTS - 1u;
}

/* Timer interrupt: read the player's stamp, keep the window, say start / stop, and hand the PLL the clocks that have fallen due. */
static void seq_feed(uint32_t now)
{
    uint32_t v = SQ_STAMP[0];
    if (!(v & 1u) && v != sq_ver) {
        uint32_t seq = SQ_STAMP[1], cyc = SQ_STAMP[2], lo = SQ_STAMP[3], hi = SQ_STAMP[4];
        if (SQ_STAMP[0] == v) {
            sq_ver = v;
            if (now - cyc <= core_cps / 10u) {                    /* a fresh stamp */
                if (!sq_prim || now - sq_prim_t > core_cps / 10u) {
                    sq_prim = seq;
                    sq_reset();
                }
                if (seq == sq_prim) {
                    sq_prim_t = cyc;
                    int64_t y = (int64_t)hi << 32 | lo;
                    uint32_t last = (sq_wr - 1u) & (SQ_N - 1);
                    if (sq_n && cyc - sq_cyc[last] < core_cps / 800u) {
                        /* two calls of one block: the second says nothing new */
                    } else {
                        sq_fit f;
                        if (sq_n >= SQ_MIN && sq_line(&f)) {      /* a jump (locate, new sequence): start the window again, keep the PLL running */
                            int64_t yp = f.now_o + (f.sy * f.sxx + (int64_t)((int32_t)((cyc - f.cyc_o) >> 8) - f.xm) * f.sxy * (int64_t)f.n) / ((int64_t)f.n * f.sxx);
                            int64_t d = y - yp;
                            if (d > 12 || d < -12) {
                                sq_restarts++;
                                sq_reset();
                                sq_resync = 1;                    /* numbers the clocks again from the new position (not 960 skips later) */
                            }
                        }
                        sq_cyc[sq_wr & (SQ_N - 1)] = cyc;
                        sq_now[sq_wr & (SQ_N - 1)] = y;
                        sq_wr++;
                        if (sq_n < SQ_N)
                            sq_n++;
                        sq_accepted++;
                    }
                }
            }
        }
    }
    /* stopped: no stamp for 50 ms, or the last four stamps say the same time */
    bool still = now - sq_prim_t > core_cps / 20u;
    if (!still && sq_n >= 4) {
        uint32_t k = (sq_wr - 1u) & (SQ_N - 1);
        still = sq_now[k] == sq_now[(k - 1u) & (SQ_N - 1)] && sq_now[k] == sq_now[(k - 2u) & (SQ_N - 1)] && sq_now[k] == sq_now[(k - 3u) & (SQ_N - 1)];
    }
    if (still) {
        sq_stopped();
        return;
    }
    sq_fit f;
    if (!sq_line(&f))
        return;
    if (!sq_running || sq_resync) {                              /* the transport has been running long enough to trust the line */
        if (!sq_running) {
            sq_running = 1;
            sq_announce = 1;
            sq_from_zero = f.now_o < 400;                        /* started from the top (the window opens within ~170 ms of the start) */
        }
        sq_resync = 0;
        int64_t newest = sq_now[(sq_wr - 1u) & (SQ_N - 1)];
        sq_next_k = newest >= 0 ? newest / 40 : -((-newest) / 40) - 1;
        if (sq_next_k * 40 < newest - 40)
            sq_next_k++;
    }
    if (sync_state >= SY_LOCKED) {
        /* The fitted slope IS the tempo, averaged over the whole window: use it as the PLL's period (smoothed 1/8 per call) instead of letting the loop re-estimate it from
           single clocks, which passes the stamps' jitter on to the tempo (it showed as a few tenths of a BPM). The loop then only corrects the phase. */
        uint64_t whole = 40ull * 256u * (uint64_t)f.sxx / (uint64_t)f.sxy;                 /* cycles per 40 ticks, whole part */
        uint64_t rem = 40ull * 256u * (uint64_t)f.sxx % (uint64_t)f.sxy;
        uint64_t target = (whole << 16) + (rem << 16) / (uint64_t)f.sxy;
        P_q16 += ((int64_t)target - (int64_t)P_q16) >> 3;
        period_from_cycles();
    }
    for (int guard = 0; guard < 8; guard++) {
        uint32_t t = sq_when(&f, sq_next_k * 40) + (uint32_t)sq_offset_cyc;
        int32_t late = (int32_t)(now - t);
        if (late < 0)
            break;                                               /* not due yet */
        if (late > (int32_t)core_cps / 2) {                      /* far behind (the line moved a lot): skip ahead instead of catching up */
            sq_next_k++;
            continue;
        }
        sq_push_clock(t);
        sq_next_k++;
    }
}

HOOK void mcl_tim6_isr(void)
{
    uint32_t now = DWT_CYCCNT;
    isr_calls++;
    T_SR = 0;                                                    /* UIF */
    if (ready != READY_MAGIC)
        return;
    if (sync_state != SY_INTERNAL) {
        if (src_seq)
            seq_feed(now);                                       /* the Blackbox's own transport: the stamps become input clocks */
        /* MIDI-in sync: first the commands and clocks that arrived since the last interrupt */
        while (cmd_rd != cmd_wr) {
            uint8_t c = in_cmd[cmd_rd++ & (IN_N - 1)];
            if (sync_state >= SY_LOCKED) {
                if (c == 0xfa) {
                    pending_start = 1;
                    pending_zero = 1;
                } else if (c == 0xfb) {
                    pending_start = 2;                           /* continue */
                } else {
                    bb_midi_tx_rt(0xfc);
                }
            }
        }
        acq_now = 0;
        while (in_rd != in_wr)
            pll_clock(in_t[in_rd++ & (IN_N - 1)], now);
        if (src_seq && sq_announce && sync_state == SY_LOCKED) {  /* the clock is up: FA with the next clock (from the top) or FB (continue) */
            pending_start = sq_from_zero ? 1u : 2u;
            pending_zero = sq_from_zero;
            sq_announce = 0;
        }
        if (acq_now)
            return;                                              /* the first interval is programmed: tick emission starts at the next interrupt */
        if (sync_state == SY_LISTEN || sync_state == SY_ONE) {
            if (sync_state == SY_ONE && now - t_last_in > core_cps / 2u)   /* the single clock was a stray */
                sync_state = SY_LISTEN;
            T_ARR = POLL_COUNTS - 1u;
            return;
        }
        if (sync_state == SY_LOCKED && now - t_last_in > 4u * (uint32_t)(P_q16 >> 16))
            sync_state = SY_LOST;                                /* no clock for four periods: hold the tempo */
        if (sync_state == SY_LOST) {
            if (++lost_ticks > FLYWHEEL_S * (timer_hz / ((uint32_t)(period_hi ? period_hi : 1u)))) {
                sync_state = SY_LISTEN;                          /* two seconds without a clock: stop and listen again */
                T_ARR = POLL_COUNTS - 1u;
                return;
            }
        }
    } else if (!running) {
        return;
    }
    /* next interval first: it must be set long before the counter gets there */
    uint64_t a = (uint64_t)acc + period_lo;
    int64_t n = (int64_t)period_hi + (int64_t)(a >> 32) + pend_adj;
    pend_adj = 0;
    acc = (uint32_t)a;
    if (n < 2)
        n = 2;
    if (n > 0x10000)
        n = 0x10000;
    T_ARR = (uint32_t)n - 1u;
    if (pending_start && (sub == 0 || sync_state == SY_INTERNAL)) {      /* a start received on the jack leaves together with the next clock */
        bb_midi_tx_rt(pending_start == 2 ? 0xfb : 0xfa);
        pending_start = 0;
    }
    if (sub == 0) {
        t_sub0 = now;
        if (pending_zero) {
            pending_zero = 0;
            ticks = 0;
        }
        bb_midi_tx_rt(0xf8);
    }
    sub = (sub + 1u) & 7u;
    ticks++;
    void (*h)(uint32_t) = tick_hook;
    if (h)
        h(ticks);
}

}
