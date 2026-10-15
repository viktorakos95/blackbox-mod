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

HOOK int bb_mcl_active(void) { return mcl_state == 2; }
HOOK const uint8_t *bb_mcl_fb(void) { return front; }
HOOK uint32_t bb_mcl_frames(void) { return mcl_frames; }
HOOK uint32_t bb_mcl_state(void) { return mcl_state; }

/* ---- MIDI between MCL's MidiUart and bb_midi ---- */
static void midi_rx_pump(void)
{
    int c;
    uint32_t guard = 0;
    while (guard++ < 256 && (c = bb_midi_rx_get()) >= 0) {
        uint8_t b = (uint8_t)c;
        if (b == 0xf8)
            continue;                                              /* the clock is bb_clock's */
        MidiUart.desktop_ingress(&b, 1);
    }
}

static void midi_tx_drain(void)
{
    volatile RingBuffer<> *rt = MidiUart.txRb_realtime;
    while (rt->rd != rt->wr) {                                    /* real-time bytes first; F8 is bb_clock's */
        uint8_t b = rt->buf[rt->rd];
        if (b != 0xf8 && !bb_midi_tx_rt(b))
            break;                                                 /* the FIFO is full: try again at the next pass */
        rt->rd = (uint16_t)(rt->rd + 1u == rt->len ? 0 : rt->rd + 1u);
    }
    volatile RingBuffer<> *tx = MidiUart.txRb;
    uint8_t tmp[64];
    uint32_t free_bytes = bb_midi_tx_free();
    for (;;) {
        uint32_t n = 0;
        while (n < sizeof tmp && n < free_bytes && tx->rd != tx->wr) {
            tmp[n++] = tx->buf[tx->rd];
            tx->rd = (uint16_t)(tx->rd + 1u == tx->len ? 0 : tx->rd + 1u);
        }
        if (!n)
            break;
        bb_midi_tx(tmp, n);
        free_bytes -= n;
    }
}

/* ---- the base timer (5 kHz) and the soft interrupts ---- */
static uint8_t sub_ms;

extern "C" void bb_base_timer_isr(void)
{
    g_clock_fast++;
    MidiClock.div192th_countdown++;
    if (MidiClock.state == MidiClockClass::STARTED && MidiClock.div192_time > 0 && MidiClock.div192th_countdown >= MidiClock.div192_time && MidiClock.interp_budget > 0) {
        MidiClock.increment192Counter();
        MidiClock.div192th_countdown = 0;
        MidiClock.interp_budget--;
        if (!MidiClock.inCallback)
            TRIGGER_SW_IRQ1();
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

HOOK void bb_swirq1_isr(void)                                      /* the sequencer step */
{
    MidiClock.inCallback = true;
    uint8_t lock = MidiUartParent::handle_midi_lock;
    MidiUartParent::handle_midi_lock = 1;
    mcl_seq.seq();
    MidiUartParent::handle_midi_lock = lock;
    MidiClock.inCallback = false;
    midi_tx_drain();
}

HOOK void bb_swirq2_isr(void)                                      /* MIDI input, and MCL's output on its way */
{
    midi_rx_pump();
    handleIncomingMidi();
    midi_tx_drain();
}

HOOK void bb_swirq3_isr(void)                                      /* the panel */
{
    GUI_hardware.poll();
}

/* ---- the task ---- */
static void mcl_task(void *)
{
    mcl_state = 1;
    setup();
    mcl_state = 2;
    for (;;) {
        loop();
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

/* Creates the task (from the UI task, once, when the runtime is up). Returns the task handle (0 = the kernel had no memory for it). */
HOOK uint32_t bb_mcl_start(uint32_t stack_words)
{
    if (mcl_state)
        return 1;
    task_def.name = "MCL";
    task_def.func = mcl_task;
    task_def.priority = 0;                                          /* osPriorityNormal: the same as the UI and the streamer (which never blocks: a lower priority would never run) */
    task_def.instances = 1;
    task_def.stack_words = stack_words;
    uint32_t h = ((fn4)(0x08087a70u | 1u))((uint32_t)&task_def, 0, 0, 0);       /* osThreadCreate */
    if (!h)
        mcl_state = 3;
    return h;
}

/* ---- the panel ---- */
HOOK void bb_mcl_page_input(uint32_t kind, int32_t a, int32_t b)
{
    if (kind == 1)
        bb_mcl_input_encoder((uint32_t)a, b);
    else if (kind == 2 || kind == 3)
        bb_mcl_input_button(a < 4 ? ButtonsClass::BUTTON1 + (uint32_t)a : ButtonsClass::FUNC_BUTTON1, kind == 2);
}
