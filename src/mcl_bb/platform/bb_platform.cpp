/* MCL's Arduino-style runtime services on the Blackbox (milestone M8): time (millis / micros), delay, soft interrupts and the base timers (irqs), the panel input.
 *
 * Interrupt design (the RP2040 / TBD port's, mapped onto the STM32H7 and FreeRTOS' rules, docs/mcl-m0-m1-results.md):
 *   USART6 (priority 0)  MIDI jack, bb_midi.cpp            TIM6 (1)  the exact MIDI clock, bb_clock.cpp
 *   TIM7 (2)             MCL's base timer, 5 kHz           soft interrupts (3): unused peripherals' vectors, pended from software
 *      soft 1 = sequencer step, soft 2 = MIDI input processing, soft 3 = GUI hardware poll (1 kHz)
 * All three soft interrupts have the same priority, so they never interrupt each other (the sequencer and the MIDI processing share MCL's state, as on the RP2040).
 * None of these may call FreeRTOS: priorities 0..4 are above what the kernel masks. MCL's own LOCK() masks priority 3 and below (bb_platform.h). */
#include "Arduino.h"
#include "bb_platform.h"
#include "bb_clock.h"

#define DWT_CYCCNT (*(volatile uint32_t *)0xe0001004u)
#define SYST_RVR   (*(volatile uint32_t *)0xe000e014u)
#define NVIC_ISER(n) (*(volatile uint32_t *)(0xe000e100u + 4u * (n)))
#define NVIC_ISPR(n) (*(volatile uint32_t *)(0xe000e200u + 4u * (n)))
#define NVIC_IPR(irq) (*(volatile uint8_t *)(0xe000e400u + (irq)))
#define RCC_APB1LENR (*(volatile uint32_t *)0x580244e8u)
#define TIM7_BASE   0x40001400u
#define T7_CR1  (*(volatile uint32_t *)(TIM7_BASE + 0x00))
#define T7_DIER (*(volatile uint32_t *)(TIM7_BASE + 0x0c))
#define T7_SR   (*(volatile uint32_t *)(TIM7_BASE + 0x10))
#define T7_EGR  (*(volatile uint32_t *)(TIM7_BASE + 0x14))
#define T7_PSC  (*(volatile uint32_t *)(TIM7_BASE + 0x28))
#define T7_ARR  (*(volatile uint32_t *)(TIM7_BASE + 0x2c))

enum { IRQ_SW1 = 30, IRQ_SW2 = 44, IRQ_SW3 = 45, IRQ_TIM7 = 55 };      /* vectors that point at `b .` stubs in the stock firmware (patches/mclblob.py gives them handlers) */

extern uint8_t SW_IRQ1, SW_IRQ2, SW_IRQ3;
uint8_t SW_IRQ1 = IRQ_SW1, SW_IRQ2 = IRQ_SW2, SW_IRQ3 = IRQ_SW3;

/* ---- time ---- */
static volatile uint32_t ms_count;                               /* incremented by the base timer's 1 kHz part */
static volatile uint32_t ms_tick_cyc;                            /* cycle counter at that increment */
static volatile bool timers_running;
static uint32_t core_cps;

extern "C" void bb_time_tick(void)                                /* from the base timer, once a millisecond */
{
    ms_tick_cyc = DWT_CYCCNT;
    ms_count = ms_count + 1u;
}

extern "C" {
unsigned long millis(void)
{
    if (timers_running)
        return ms_count;
    return (unsigned long)((uint64_t)DWT_CYCCNT * 1000u / ((SYST_RVR + 1u) * 1000u));     /* before the timers (setup's first calls): wraps after 10 s, nothing waits on it that long */
}

unsigned long micros(void)
{
    uint32_t cps = (SYST_RVR + 1u) * 1000u;
    if (!timers_running)
        return (unsigned long)((uint64_t)DWT_CYCCNT * 1000000u / cps);
    uint32_t ms, tc, now;
    do {
        ms = ms_count;
        tc = ms_tick_cyc;
        now = DWT_CYCCNT;
    } while (ms != ms_count);
    uint32_t frac = (uint32_t)((uint64_t)(now - tc) * 1000000u / cps);          /* microseconds since the last millisecond tick */
    if (frac > 999u)
        frac = 999u;
    return (unsigned long)(ms * 1000u + frac);
}

void delayMicroseconds(unsigned int us)
{
    uint32_t t0 = DWT_CYCCNT, n = (uint32_t)((uint64_t)us * ((SYST_RVR + 1u) * 1000u) / 1000000u);
    while (DWT_CYCCNT - t0 < n) {}
}
}

/* A real sleep in a task: wait on a semaphore nobody gives, with a time-out (the stock FreeRTOS tick is 1 kHz). The stock functions are Thumb at fixed addresses (docs M7). */
typedef uint32_t (*fn4)(uint32_t, uint32_t, uint32_t, uint32_t);
static uint32_t sleep_sem;

extern "C" void bb_sleep_ms(uint32_t ms)
{
    if (bb_in_interrupt() || !timers_running) {                   /* no scheduler to sleep in */
        delayMicroseconds(ms * 1000u);
        return;
    }
    if (!sleep_sem)
        sleep_sem = ((fn4)(0x080883beu | 1u))(1, 0, 3, 0);        /* xQueueGenericCreate: a binary semaphore */
    if (!sleep_sem) {
        delayMicroseconds(ms * 1000u);
        return;
    }
    ((fn4)(0x08088756u | 1u))(sleep_sem, 0, ms, 0);               /* xQueueGenericReceive with a time-out */
}

extern "C" void delay(unsigned long ms) { bb_sleep_ms((uint32_t)ms); }

void platform_poll() {}
void platform_wait_poll() {}
bool mcl_platform_headless_boot() { return false; }

/* ---- soft interrupts ---- */
extern "C" void bb_swirq_pend(int n)
{
    switch (n) {
    case 1: NVIC_ISPR(0) = 1u << IRQ_SW1; break;
    case 2: NVIC_ISPR(1) = 1u << (IRQ_SW2 - 32); break;
    case 3: NVIC_ISPR(1) = 1u << (IRQ_SW3 - 32); break;
    }
}

/* ---- the panel ---- */
static volatile uint32_t buttons_lo, buttons_hi;
static volatile int32_t enc_acc[4];
static volatile uint32_t enc_buttons;

extern "C" void bb_mcl_input_button(uint32_t id, int down)
{
    uint32_t lk = bb_lock();
    if (id < 32) {
        if (down) buttons_lo |= 1u << id; else buttons_lo &= ~(1u << id);
    } else if (id < 64) {
        if (down) buttons_hi |= 1u << (id - 32); else buttons_hi &= ~(1u << (id - 32));
    }
    bb_unlock(lk);
}

extern "C" void bb_mcl_input_encoder(uint32_t n, int detents)
{
    if (n < 4) {
        uint32_t lk = bb_lock();
        enc_acc[n] = enc_acc[n] + detents;
        bb_unlock(lk);
    }
}

uint64_t mcl_platform_button_mask() { return (uint64_t)buttons_hi << 32 | buttons_lo; }
uint32_t mcl_platform_encoder_button_mask() { return enc_buttons; }

int mcl_platform_encoder_delta(uint8_t n)
{
    if (n >= 4)
        return 0;
    uint32_t lk = bb_lock();
    int32_t v = enc_acc[n];
    enc_acc[n] = 0;
    bb_unlock(lk);
    return (int)v;
}

/* ---- the base timer and the soft interrupts: set up by MCL's setup() (setup_irqs) ---- */
extern "C" void bb_base_timer_isr(void);                          /* bb_mcl.cpp: the work of one 5 kHz tick */

extern "C" __attribute__((section(".hook"), used)) void bb_tim7_isr(void)
{
    T7_SR = 0;
    bb_base_timer_isr();
}

void setup_irqs()
{
    if (timers_running)
        return;
    bb_clock_init();                                              /* the cycle counter against the timers' clock: needed before TIM7 (same clock tree as TIM6) */
    uint32_t hz = bb_clock_timer_hz();
    if (!hz)
        return;
    core_cps = (SYST_RVR + 1u) * 1000u;
    ms_tick_cyc = DWT_CYCCNT;
    NVIC_IPR(IRQ_TIM7) = 0x20;
    NVIC_IPR(IRQ_SW1) = 0x30;
    NVIC_IPR(IRQ_SW2) = 0x30;
    NVIC_IPR(IRQ_SW3) = 0x30;
    RCC_APB1LENR |= 0x20u;                                         /* TIM7 */
    (void)RCC_APB1LENR;
    T7_CR1 = 0;
    T7_DIER = 0;
    T7_PSC = bb_clock_prescaler() - 1u;                            /* the same ~1 MHz count as TIM6 */
    T7_ARR = hz / 5000u - 1u;                                      /* 200 counts: 5 kHz */
    T7_EGR = 1;
    T7_SR = 0;
    timers_running = true;
    NVIC_ISER(0) = 1u << IRQ_SW1;
    NVIC_ISER(1) = (1u << (IRQ_SW2 - 32)) | (1u << (IRQ_SW3 - 32)) | (1u << (IRQ_TIM7 - 32));
    T7_DIER = 1;
    T7_CR1 = 1;
}
