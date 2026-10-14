/*
 * MIDI clock source for MCL (milestone M4): a hardware timer, independent of the 128-frame audio block (2.67 ms) and of the stock output queue.
 *
 * TIM6 (a basic 16-bit timer, vector 54; found unused in the stock firmware: no handler, no references) counts at about 1 MHz and interrupts on every
 * update event. The interrupt sets the length of the NEXT interval from a 32.32 fixed-point accumulator (period = 60 * rate / (192 * bpm) timer counts),
 * so the whole-count rounding error is carried to the following tick instead of lost: over any run the tick times are the ideal ones to within one
 * timer count, and an interrupt that is served late does not move the next one (the counter reloads by itself). Every 8th tick (24 PPQN) a 0xF8 goes to
 * MCL's TX ring as a real-time byte, which leaves at the next byte boundary even in the middle of a sysex dump. Priority 1: above the audio DMA
 * (5..9) and FreeRTOS' critical sections (>= 5), below the USART6 interrupt (0). No FreeRTOS call, no stock queue.
 *
 * The timer's input clock is not assumed: bb_clock_init() counts it against the DWT cycle counter (core clock from the SysTick reload, the firmware's 1 kHz tick).
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
static volatile uint32_t running;
static volatile uint32_t sub;                                   /* 0..7: 192-PPQN ticks since the last 24-PPQN clock */
static volatile uint32_t ticks;
static volatile uint32_t isr_calls;                             /* every entry of the timer interrupt, ready or not (diagnostics) */
static volatile uint32_t pending_start;                         /* send FA at the next tick */
static uint32_t timer_hz;                                       /* counts per second after the prescaler */
static volatile uint32_t period_hi, period_lo;                  /* 32.32 counts per 192-PPQN tick */
static volatile uint32_t acc;                                   /* fractional part carried between ticks */
static void (*volatile tick_hook)(uint32_t);

extern "C" {

HOOK void bb_clock_down(void) { ready = 0; running = 0; isr_calls = 0; ticks = 0; }       /* TIM6 itself is not touched: its interrupt returns at once while ready is off */

static uint32_t core_hz(void)
{
    uint32_t hz = (SYST_RVR + 1u) * 1000u;                       /* the FreeRTOS tick is 1 kHz */
    return (SYST_CSR & 4u) ? hz : hz * 8u;                       /* CLKSOURCE: 1 = core clock, 0 = core / 8 */
}

HOOK int bb_clock_init(void)
{
    ready = 0;
    running = 0;
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
    uint64_t hz = (uint64_t)core_hz() * counts / cycles;         /* timer clock at PSC = 0 */
    uint32_t psc = (uint32_t)((hz + 500000u) / 1000000u);        /* prescaler divisor for about 1 MHz */
    if (psc < 1)
        psc = 1;
    T_PSC = psc - 1u;
    timer_hz = (uint32_t)(hz / psc);
    T_EGR = 1;
    T_SR = 0;
    NVIC_IPR54 = 0x10;                                           /* priority 1 (4 bits used, top-aligned) */
    return timer_hz > 100000u && timer_hz < 10000000u;
}

HOOK uint32_t bb_clock_timer_hz(void) { return timer_hz; }

HOOK void bb_clock_tempo(uint32_t bpm_x10)
{
    if (bpm_x10 < 200u)
        bpm_x10 = 200u;
    if (bpm_x10 > 3000u)
        bpm_x10 = 3000u;
    /* counts per 192-PPQN tick = 60 * hz / (192 * bpm) = 600 * hz / (192 * bpm_x10), as 32.32 */
    uint64_t q = ((uint64_t)600u * timer_hz << 32) / (192ull * bpm_x10);
    period_hi = (uint32_t)(q >> 32);
    period_lo = (uint32_t)q;
}

HOOK void bb_clock_start(uint32_t bpm_x10, int send_start)
{
    if (!timer_hz)
        return;
    T_CR1 = 0;
    T_DIER = 0;
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

HOOK void bb_clock_stop(int send_stop)
{
    T_DIER = 0;
    T_CR1 = 0;
    running = 0;
    if (send_stop)
        for (uint32_t spin = 0; spin < 200000u && !bb_midi_tx_rt(0xfc); spin++) {}      /* the real-time FIFO drains at 31250 baud: wait for a slot (GUI task, ~ms) */
}

HOOK uint32_t bb_clock_isr_calls(void) { return isr_calls; }
HOOK int bb_clock_running(void) { return (int)running; }
HOOK uint32_t bb_clock_ticks(void) { return ticks; }
HOOK void bb_clock_set_hook(void (*tick)(uint32_t)) { tick_hook = tick; }

HOOK void mcl_tim6_isr(void)
{
    isr_calls++;
    T_SR = 0;                                                    /* UIF */
    if (ready != READY_MAGIC || !running)
        return;
    /* next interval first: it must be set long before the counter gets there */
    uint64_t a = (uint64_t)acc + period_lo;
    uint32_t n = period_hi + (uint32_t)(a >> 32);
    acc = (uint32_t)a;
    if (n > 0x10000u)
        n = 0x10000u;
    T_ARR = n - 1u;
    if (pending_start) {
        pending_start = 0;
        bb_midi_tx_rt(0xfa);
    }
    if (sub == 0)
        bb_midi_tx_rt(0xf8);
    sub = (sub + 1u) & 7u;
    ticks++;
    void (*h)(uint32_t) = tick_hook;
    if (h)
        h(ticks);
}

}
