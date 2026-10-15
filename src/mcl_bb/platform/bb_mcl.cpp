/* MCL running on the Blackbox (milestone M8): the glue between MCL's core and the Blackbox layers.
 *   - a FreeRTOS task of its own runs MCL's setup() and then loop() (the stock task stacks are too small and shared with the stock UI);
 *   - the 5 kHz base timer's work (bb_base_timer_isr) and the three soft interrupts are MCL's RP2040 timer / software-IRQ structure (src/platform/rp2040/irqs.cpp);
 *   - MIDI: bytes from the jack (bb_midi) are fed to MidiUart, MidiUart's output rings are drained into bb_midi's; the clock byte F8 is NOT passed either way, the
 *     exact clock is bb_clock's (TIM6), which drives MidiClock's counters through bb_clock's tick hook (M8, clock step);
 *   - the screen: MCL's Oled buffer is copied to a front buffer when MCL calls display() and shown by the page; the panel input comes from the page. */
#include "Arduino.h"
#include "platform.h"
#include "bb_platform.h"
#include "MidiClock.h"
#include "MidiUart.h"
#include "Sequencer/MCLSeq.h"
#include "GUI_hardware.h"
#include "GUI.h"
#include "global.h"
#include "oled.h"

#include "bb_clock.h"
#include "bb_midi.h"
#include "bb_sd.h"

#define HOOK extern "C" __attribute__((section(".hook"), used))

extern void setup();
extern void loop();
extern "C" void bb_time_tick(void);
extern "C" void bb_sleep_ms(uint32_t ms);
void handleIncomingMidi();

static volatile uint32_t mcl_state;                              /* 0 not started, 1 starting (setup running), 2 running, 3 failed to start */
static volatile uint32_t mcl_frames;
extern "C" volatile uint32_t bb_dbg[16];                           /* progress counters (bb_platform.cpp defines them): 0 task stage, 1 delay calls, 2 last delay ms, 3 setup_irqs entered, 4 ... got its timer, 5 TIM7 interrupts, 6/7/8 soft interrupts 1/2/3, 9 loop passes, 10 sleep semaphore missing */
static volatile uint32_t mcl_stack;                              /* the stack the task got, words */
static uint8_t front[OLED_WIDTH * OLED_HEIGHT / 8];

/* ---- the screen ---- */
extern "C" void __real__ZN4Oled7displayEv(Oled *self);
extern "C" void __wrap__ZN4Oled7displayEv(Oled *self)
{
    __real__ZN4Oled7displayEv(self);                               /* (draws the text box overlay into the buffer) */
    const uint8_t *src = self->getBuffer();
    for (unsigned i = 0; i < sizeof front; i++)
        front[i] = src[i];
    mcl_frames = mcl_frames + 1u;
}

HOOK int bb_mcl_active(void) { return mcl_state == 1 || mcl_state == 2; }   /* (setup() itself shows the project menu and waits there: state 1 is a running MCL too) */
HOOK const uint8_t *bb_mcl_fb(void) { return front; }
HOOK uint32_t bb_mcl_frames(void) { return mcl_frames; }
HOOK uint32_t bb_mcl_state(void) { return mcl_state; }
HOOK uint32_t bb_mcl_stack_words(void) { return mcl_stack; }
HOOK uint32_t bb_mcl_dbg(int i) { return i >= 0 && i < 32 ? bb_dbg[i] : 0; }
/* the kernel's view of MCL's task (diagnostics): 0 HAL tick (ms), 1 the kernel's tick count (xTickCount, the getter at 0x08088d20), 2 MCL's task handle (= its TCB), 10 + n word n of that TCB */
HOOK uint32_t bb_mcl_kern(int i)
{
    uint32_t tcb = bb_dbg[13];
    switch (i) {
    case 0: return ((uint32_t (*)(void))0x0807794du)();
    case 1: return ((uint32_t (*)(void))0x08088d21u)();
    case 2: return tcb;
    case 3: return *(volatile uint32_t *)0x2401f2a8u;                 /* pxCurrentTCB */
    case 4: { volatile uint32_t *c = (volatile uint32_t *)*(volatile uint32_t *)0x2401f2a8u; return c ? c[4] : 0; }   /* its owner word (= itself in a TCB) */
    case 5: {                                                         /* number of tasks, ready list lengths of priorities 0..4 (nibbles) */
        uint32_t v = (*(volatile uint32_t *)0x2401f1a8u & 15u) << 20;
        for (uint32_t p = 0; p < 5; p++)
            v |= (*(volatile uint32_t *)(0x2401f21cu + 0x14u * p) & 15u) << (16 - 4 * p);
        return v;
    }
    default: return tcb && i >= 10 && i < 30 ? *(volatile uint32_t *)(tcb + 4u * (uint32_t)(i - 10)) : 0;
    }
}

/* ---- MIDI between MCL's MidiUart and bb_midi ---- */
static volatile uint32_t st_rx_in, st_rx_clk, st_tx_out, st_tx_clk;     /* MIDI bytes: into MCL, clock / transport dropped from the jack, out to the jack, clock / transport dropped from MCL's output */
static volatile uint8_t xq[8];                                    /* transport bytes (FA / FB / FC) from bb_clock on their way to MCL's receive ring */
static volatile uint8_t xq_wr, xq_rd;

static inline uint32_t clk_lock(void)                             /* masks TIM6's interrupt (priority 1) and below for a few cycles */
{
    uint32_t old;
    __asm volatile("mrs %0, basepri\n msr basepri, %1" : "=&r"(old) : "r"(0x10u) : "memory");
    return old;
}
static inline void clk_unlock(uint32_t old) { __asm volatile("msr basepri, %0" ::"r"(old) : "memory"); }

static void midi_rx_pump(void)
{
    int c;
    uint32_t guard = 0;
    while (xq_rd != xq_wr) {                                      /* the transport of the Blackbox's clock, in order with the bytes from the jack */
        uint8_t b = xq[xq_rd & 7u];
        xq_rd = (uint8_t)(xq_rd + 1u);
        MidiUartClass *u = MidiClock.uart_transport_recv1 ? MidiClock.uart_transport_recv1 : &MidiUart;
        uint32_t lk = clk_lock();
        u->rxRb->put_h_isr(b);
        clk_unlock(lk);
    }
    while (guard++ < 256 && (c = bb_midi_rx_get()) >= 0) {
        uint8_t b = (uint8_t)c;
        if (b >= 0xf8 && b <= 0xfc) {
            st_rx_clk++;
            continue;                                              /* clock and transport are the Blackbox's (bb_clock): not the jack's */
        }
        st_rx_in++;
        uint32_t lk = clk_lock();                                  /* (the transport side also writes this ring, from the clock interrupt) */
        MidiUart.desktop_ingress(&b, 1);
        clk_unlock(lk);
    }
}

static void midi_tx_drain(void)
{
    volatile RingBuffer<> *rt = MidiUart.txRb_realtime;
    while (rt->rd != rt->wr) {                                    /* real-time bytes first; F8 is bb_clock's */
        uint8_t b = rt->buf[rt->rd];
        if (b < 0xf8 || b > 0xfc) {                                /* (clock and transport out are bb_clock's) */
            if (!bb_midi_tx_rt(b))
                break;                                             /* the FIFO is full: try again at the next pass */
            st_tx_out++;
        } else {
            st_tx_clk++;
        }
        rt->rd = (uint16_t)(rt->rd + 1u == rt->len ? 0 : rt->rd + 1u);
    }
    volatile RingBuffer<> *tx = MidiUart.txRb;
    uint8_t tmp[64];
    uint32_t free_bytes = bb_midi_tx_free();
    for (;;) {
        uint32_t n = 0;
        while (n < sizeof tmp && n < free_bytes && tx->rd != tx->wr) {
            uint8_t b = tx->buf[tx->rd];
            tx->rd = (uint16_t)(tx->rd + 1u == tx->len ? 0 : tx->rd + 1u);
            if (b >= 0xf8 && b <= 0xfc) {
                st_tx_clk++;
                continue;                                          /* MCL's own start / stop / forwards in the stream: the Blackbox's transport is the master */
            }
            tmp[n++] = b;
        }
        if (!n)
            break;
        bb_midi_tx(tmp, n);
        st_tx_out += n;
        free_bytes -= n;
    }
}

/* ---- the base timer (5 kHz) and the soft interrupts ---- */
static uint8_t sub_ms;

extern "C" void bb_base_timer_isr(void)
{
    g_clock_fast++;
    /* MCL's own interpolation between two MIDI clocks (what its RP2040 port does at 5 kHz): `interp_budget` more 1/(24 * clock_interpolation) ticks, spaced div192_time
     * timer ticks apart, counted from the MIDI clock that bb_clock's interrupt (priority 1) hands over exactly. That interrupt also changes these counters, so
     * it is held off for the few cycles of the update. */
    uint32_t lk = clk_lock();
    MidiClock.div192th_countdown++;
    if (MidiClock.state == MidiClockClass::STARTED && MidiClock.div192_time > 0 && MidiClock.div192th_countdown >= MidiClock.div192_time && MidiClock.interp_budget > 0) {
        MidiClock.increment192Counter();
        MidiClock.div192th_countdown = 0;
        MidiClock.interp_budget--;
        clk_unlock(lk);
        if (!MidiClock.inCallback)
            TRIGGER_SW_IRQ1();
    } else {
        clk_unlock(lk);
    }
    if (!MidiUartParent::handle_midi_lock)
        TRIGGER_SW_IRQ2();
    if (++sub_ms == 5) {
        sub_ms = 0;
        bb_time_tick();
        g_clock_ms++;
        g_clock_ticks++;
        if (g_clock_ticks == 60000) {
            g_clock_ticks = 0;
            g_clock_minutes++;
        }
        MidiUart.tickActiveSense();
        TRIGGER_SW_IRQ3();
    }
}

/* The three soft interrupts run MCL's sequencer, MIDI processing and panel poll in interrupt context, like its RP2040 port. MCL's calls go deep (a sequencer step through
 * the engines, the MIDI parser's callbacks), and the stock firmware's main stack (what an interrupt runs on) is of an unknown size, so the handlers switch to a stack
 * of their own, in the blob's RAM: they have all the same priority and never nest, so one stack serves the three; interrupts that come in on top (TIM7, TIM6, the stock
 * audio, ...) use it too, hence the margin. */
#define ISR_STACK 6144
static uint8_t isr_stack[ISR_STACK + 8] __attribute__((aligned(8)));
extern "C" uint8_t *const bb_isr_stack_top = isr_stack + ISR_STACK;

#define TRAMPOLINE(name, body)                                                                                                                       \
    extern "C" void body(void);                                                                                                                      \
    extern "C" __attribute__((naked, section(".hook"), used)) void name(void)                                                                         \
    {                                                                                                                                                \
        asm volatile("push {r4, lr}\n mov r4, sp\n ldr r0, =bb_isr_stack_top\n ldr r0, [r0]\n mov sp, r0\n bl " #body "\n mov sp, r4\n pop {r4, pc}\n"); \
    }

extern "C" void bb_swirq1_body(void)                               /* the sequencer step */
{
    bb_dbg[6]++;
    MidiClock.inCallback = true;
    uint8_t lock = MidiUartParent::handle_midi_lock;
    MidiUartParent::handle_midi_lock = 1;
    mcl_seq.seq();
    MidiUartParent::handle_midi_lock = lock;
    MidiClock.inCallback = false;
    midi_tx_drain();
}

extern "C" void bb_swirq2_body(void)                               /* MIDI input, and MCL's output on its way */
{
    bb_dbg[7]++;
    midi_rx_pump();
    handleIncomingMidi();
    midi_tx_drain();
}

extern "C" void bb_swirq3_body(void)                               /* the panel */
{
    bb_dbg[8]++;
    GUI_hardware.poll();
}

TRAMPOLINE(bb_swirq1_isr, bb_swirq1_body)
TRAMPOLINE(bb_swirq2_isr, bb_swirq2_body)
TRAMPOLINE(bb_swirq3_isr, bb_swirq3_body)

HOOK uint32_t bb_mcl_isr_stack_used(void)                           /* diagnostics: bytes of the soft interrupts' stack that were ever touched (the stack is painted at boot) */
{
    uint32_t n = ISR_STACK;
    for (uint32_t i = 0; i < ISR_STACK && isr_stack[i] == 0xa5; i++)
        n--;
    return n;
}

/* ---- the clock: bb_clock's two hooks (its timer interrupt, priority 1). The Blackbox is the master: bb_clock follows its sequencer (internal, TRS MIDI or analog sync
 * alike) and sends F8 / FA / FB / FC to the jack at exactly the right times; MCL is told through MidiClock the way its UART interrupt would tell it (MidiClockClass::
 * handleImmediateClock / handleImmediateMidiStart / ...), minus the forwards to the jack (they would duplicate bb_clock's bytes). ---- */
extern "C" void bb_mcl_clk_transport(uint8_t b)
{
    switch (b) {
    case 0xfa:                                                     /* = handleImmediateMidiStart without the forward and without the receive ring (the soft interrupt does that) */
        MidiClock.reset_clock_phase = true;
        MidiClock.init();
        MidiClock.state = MidiClockClass::STARTING;
        MidiClock.onMidiStartImmediateCallbacks.call(MidiClock.div96th_counter);
        break;
    case 0xfb:
        MidiClock.reset_clock_phase = true;
        MidiClock.state = MidiClockClass::STARTED;
        MidiClock.isInit = false;
        break;
    case 0xfc:
        MidiClock.state = MidiClockClass::PAUSED;
        break;
    default:
        return;
    }
    uint32_t lk = bb_lock();                                       /* (a task may call too: stop / restart of the clock) */
    xq[xq_wr & 7u] = b;
    xq_wr = (uint8_t)(xq_wr + 1u);
    bb_unlock(lk);
    TRIGGER_SW_IRQ2();
}

/* every 192-PPQN tick (8 to a MIDI clock): the first is the MIDI clock itself (the others are not used: MCL interpolates between clocks itself, at its 5 kHz base timer) */
extern "C" void bb_mcl_clk_tick(uint32_t ticks)
{
    if ((ticks - 1u) & 7u)
        return;
    MidiClock.uart_clock_forward1 = nullptr;                       /* (cfg_clock_recv sets them back to the jack whenever MCL's settings are applied) */
    MidiClock.uart_clock_forward2 = nullptr;
    MidiClock.uart_clock_forward3 = nullptr;
    MidiClock.handleImmediateClock();
    if (MidiClock.state == MidiClockClass::STARTED && !MidiClock.inCallback)
        TRIGGER_SW_IRQ1();
}

/* MIDI between MCL and the jack (diagnostics and tests): 0 bytes into MCL, 1 clock / transport bytes from the jack dropped, 2 bytes out, 3 clock / transport bytes of MCL's output dropped */
HOOK uint32_t bb_mcl_midi_stat(int which)
{
    switch (which) {
    case 0: return st_rx_in;
    case 1: return st_rx_clk;
    case 2: return st_tx_out;
    default: return st_tx_clk;
    }
}

/* a byte into MCL's output the way its own code puts it (tests): realtime ring or the stream */
HOOK void bb_mcl_midi_put(uint32_t b, uint32_t realtime)
{
    if (realtime)
        MidiUart.sendRealtime((uint8_t)b);
    else
        MidiUart.sendRaw((uint8_t)b);
    TRIGGER_SW_IRQ2();
}

/* the clock as MCL sees it (diagnostics and tests): 0 state (0 paused, 1 starting, 2 started), 1 div192th_counter, 2 div96th_counter, 3 div16th_counter, 4 interpolation, 5 transport bytes queued for MCL */
HOOK uint32_t bb_mcl_clk_stat(int which)
{
    switch (which) {
    case 0: return (uint32_t)MidiClock.state;
    case 1: return MidiClock.div192th_counter;
    case 2: return MidiClock.div96th_counter;
    case 3: return MidiClock.div16th_counter;
    case 4: return MidiClock.clock_interpolation;
    default: return (uint8_t)(xq_wr - xq_rd);
    }
}

/* ---- the task ---- */
static void mcl_task(void *)
{
    {
        uint32_t *c = (uint32_t *)*(volatile uint32_t *)0x2401f2a8u;     /* (diagnostics: the kernel's view of this task) */
        bb_dbg[17] = (uint32_t)c;
        bb_dbg[18] = c[4] == (uint32_t)c;
    }
    for (uint32_t i = 0; i < ISR_STACK; i++)
        isr_stack[i] = 0xa5;                                       /* (painted, to see how much the soft interrupts ever use) */
    bb_dbg[0] = 1;
    mcl_state = 1;
    bb_midi_claim(1);                                              /* MCL writes the jack (the stock firmware's own sends are redirected into the same ring) */
    if (!bb_midi_owner())
        bb_midi_claim(0);
    bb_dbg[0] = 2;
    setup();                                                       /* (with the project menu: it returns when a project is loaded) */
    bb_dbg[0] = 3;
    mcl_state = 2;
    bb_clock_set_transport_hook(bb_mcl_clk_transport);
    bb_clock_set_hook(bb_mcl_clk_tick);
    bb_clock_sync_seq();                                           /* the clock follows the Blackbox's sequencer */
    bb_midi_filter_clock(1);                                       /* ... and sends the clock and transport out: the stock firmware's own F8 / FA / FB / FC are dropped from here on
                                                                      (until then, in the start menu, they go out as before) */
    for (;;) {
        bb_dbg[9]++;
        loop();
        if (!bb_midi_owner())
            bb_midi_claim(1);                                      /* (still waiting for an idle moment of the stock transmitter) */
        bb_sleep_ms(1);
    }
}

typedef uint32_t (*fn4)(uint32_t, uint32_t, uint32_t, uint32_t);
struct ThreadDef {                                                 /* CMSIS-RTOS v1 osThreadDef_t, as the stock firmware's main() uses it */
    const char *name;
    void (*func)(void *);
    int32_t priority;
    uint32_t instances;
    uint32_t stack_words;
};
static ThreadDef task_def;

/* Creates the task (from the UI task, once, when the runtime is up). Returns the task handle (0 = the kernel had no memory for it, even for the smaller stack tried).
 * The stack comes from the kernel's heap, whose free size is not known here: ask for `stack_words` and, if that is refused, for less (7/8 of it; MCL needs about 13 KB). */
HOOK uint32_t bb_mcl_start(uint32_t stack_words)
{
    if (mcl_state)
        return 1;
    task_def.name = "MCL";
    task_def.func = mcl_task;
    task_def.priority = 0;                                          /* osPriorityNormal, as the UI and the streamer tasks (a higher priority did not change the stuck sleep seen on the unit) */
    task_def.instances = 1;
    uint32_t h = 0;
    mcl_state = 1;                                                  /* (before the task exists: the caller must not get here twice) */
    for (uint32_t i = 0; i < 2 && !h; i++) {
        static const uint8_t num[2] = {8, 7};
        task_def.stack_words = stack_words * num[i] / 8;
        h = ((fn4)(0x08087a70u | 1u))((uint32_t)&task_def, 0, 0, 0);       /* osThreadCreate */
    }
    if (!h)
        mcl_state = 3;
    else {
        mcl_stack = task_def.stack_words;
        bb_dbg[13] = h;
        bb_dbg[16] = ((volatile uint32_t *)h)[4] == h;                                             /* the task handle = its TCB (diagnostics) */
    }
    return h;
}

/* ---- the panel ---- */
extern "C" int bb_mcl_enc_units(uint8_t n)
{
    LightPage *p = GUI.encoderPage();
    Encoder *e = p && n < GUI_NUM_ENCODERS ? p->encoders[n] : nullptr;
    return (e ? e->rot_res : 1) * ENCODER_RES_MULTIPLIER + 1;
}

HOOK void bb_mcl_page_input(uint32_t kind, int32_t a, int32_t b)
{
    if (kind == 1)
        bb_mcl_input_encoder((uint32_t)a, b);
    else if (kind == 2 || kind == 3)
        bb_mcl_input_button(a < 4 ? ButtonsClass::BUTTON1 + (uint32_t)a : ButtonsClass::FUNC_BUTTON1, kind == 2);
}
