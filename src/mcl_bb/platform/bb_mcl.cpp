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
#include "MD.h"
#include "MCL.h"
#include "MDPages.h"
#include "GUI/Pages/Sequencer/SeqPages.h"
#include "GUI/Pages/Sequencer/PolyPage.h"
#include "GUI/Pages/Grid/GridPages.h"
#include "Devices/DeviceManager.h"
#include "bb_grid.h"
#include "GUI/Pages/CommonPages.h"
#include "Drivers/MD/UI/Pages/RAMPage.h"
#include "Sequencer/BeatRepeat.h"
#include "NoteInterface.h"

/* the SAMPLE button's player and cable mode (see bb_sample_record below) */
extern "C" {
struct bb_player *bb_player(void);
int bb_player_open(const char *name);
void bb_player_close(void);
void bb_player_play(void);
void bb_player_stop(void);
void bb_player_pump(void);
}
extern "C" volatile uint8_t bb_cable_mode, bb_cable_listen;     /* (the fork's SampleBrowserPage reads them: fork patch 0004) */
volatile uint8_t bb_cable_mode, bb_cable_listen;
static volatile uint8_t cable_armed;                             /* the file is loaded and waits for the record's first step */
static volatile uint16_t cable_step;                             /* that step (RAMPage's transition_step) */

#include "bb_clock.h"
#include "bb_midi.h"
#include "bb_sd.h"

#define HOOK extern "C" __attribute__((section(".hook"), used))
#define DWT_CYCCNT (*(volatile uint32_t *)0xe0001004u)
#define SYST_RVR   (*(volatile uint32_t *)0xe000e014u)

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
    case 40: case 41: {                                               /* the run of zero words around MCL's TCB: 40 its lowest address, 41 the first non-zero word at or above the TCB (<= 64 KB scanned each way) */
        volatile uint32_t *w = (volatile uint32_t *)tcb;
        if (!tcb)
            return 0;
        uint32_t n = 0;
        if (i == 40) {
            while (n < 16384u && w[-(int32_t)n - 1] == 0)
                n++;
            return tcb - 4u * n;
        }
        while (n < 16384u && w[n] == 0)
            n++;
        return tcb + 4u * n;
    }
    case 5: {                                                         /* number of tasks, ready list lengths of priorities 0..4 (nibbles) */
        uint32_t v = (*(volatile uint32_t *)0x2401f1a8u & 15u) << 20;
        for (uint32_t p = 0; p < 5; p++)
            v |= (*(volatile uint32_t *)(0x2401f21cu + 0x14u * p) & 15u) << (16 - 4 * p);
        return v;
    }
    default: return tcb && i >= 10 && i < 30 ? *(volatile uint32_t *)(tcb + 4u * (uint32_t)(i - 10)) : 0;
    }
}

/* Called every frame while the diagnostics show: is MCL's TCB still a TCB (owner word = itself), and when did that last / first not hold; the kernel's task count then (diagnostics) */
HOOK void bb_mcl_guard(void)
{
    uint32_t h = bb_dbg[13];
    if (!h)
        return;
    uint32_t now = ((uint32_t (*)(void))0x0807794du)(), n = *(volatile uint32_t *)0x2401f1a8u;
    if (!bb_dbg[25])
        bb_dbg[25] = now, bb_dbg[20] = n;                               /* creation: tick, task count */
    if (((volatile uint32_t *)h)[4] == h)
        bb_dbg[21] = now - bb_dbg[25];                                  /* last time it was fine (ms after creation) */
    else if (!bb_dbg[23])
        bb_dbg[23] = now - bb_dbg[25] + 1u, bb_dbg[24] = n;             /* first time it was not (+1 so that 0 means "not yet"), the task count then */
}

/* ---- MIDI between MCL's MidiUart and bb_midi ---- */
static volatile uint32_t st_rx_in, st_rx_clk, st_tx_out, st_tx_clk, st_ext_in, st_ext_clk;     /* MIDI bytes: into MCL, clock / transport dropped from the jack, out to the jack, clock / transport dropped from MCL's output */
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
            MidiUart.recvActiveSenseTimer = 0;                     /* (still a sign of life of the device on the jack, as any byte is on MCL's own hardware: peering) */
            continue;                                              /* clock and transport are the Blackbox's (bb_clock): not the jack's */
        }
        st_rx_in++;
        uint32_t lk = clk_lock();                                  /* (the transport side also writes this ring, from the clock interrupt) */
        MidiUart.desktop_ingress(&b, 1);
        clk_unlock(lk);
    }
    /* MCL's port 2: what the Blackbox takes from its software MIDI-in queue (a keyboard on the USB host port, Trigger2MIDI's INT output) */
    for (guard = 0; guard < 256 && (c = bb_midi_ext_get()) >= 0; guard++) {
        uint8_t b = (uint8_t)c;
        if (b >= 0xf8 && b <= 0xfc) {
            st_ext_clk++;
            continue;                                              /* (the Blackbox is the clock master here too) */
        }
        st_ext_in++;
        MidiUart2.desktop_ingress(&b, 1);
    }
}

/* MCL's output to the jack, in the order its UART interrupt sends it on its own hardware (MidiUart::desktop_egress): real-time bytes first, then the
 * sequencer's side channel (seq_tx1..4: MCLSeq::seq() writes its notes there and points txRb_sidechannel at it) but only between two messages of the
 * main stream, then the main stream. F8 / FA / FB / FC are dropped: bb_clock sends the Blackbox's clock and transport. */
static void midi_tx_drain(void)
{
    uint8_t tmp[64];
    for (int guard = 0; guard < 128; guard++) {
        uint32_t free_bytes = bb_midi_tx_free();
        uint32_t cap = free_bytes < sizeof tmp ? free_bytes : (uint32_t)sizeof tmp;
        if (!cap)
            break;                                                 /* the jack's ring is full: the rest waits for the next pass */
        size_t k = MidiUart.desktop_egress(tmp, cap);
        if (!k)
            break;
        uint32_t n = 0;
        for (size_t i = 0; i < k; i++) {
            uint8_t b = tmp[i];
            if (b >= 0xf8 && b <= 0xfc) {
                st_tx_clk++;
                continue;
            }
            tmp[n++] = b;
        }
        if (n) {
            bb_midi_tx(tmp, n);
            st_tx_out += n;
        }
    }
}

/* TurboMIDI (TurboLight::set_speed: the speed command and 16 pad bytes are queued, then set_speed). The jack's object waits until its own rings have been
 * handed to bb_midi (soft interrupt 2 does that, every 0.2 ms), then bb_midi waits until they have left the UART at the old speed and switches. A port that is
 * not the jack (USB) keeps the plain bookkeeping. 50 ms each at most: then it switches anyway (the other side switches after the pad bytes in any case). */
void MidiUartClass::set_speed(uint32_t bps)
{
    if (!bps)
        bps = 31250;
    if (this != &MidiUart || bb_in_interrupt()) {
        speed = bps;
        return;
    }
    uint32_t t0 = DWT_CYCCNT, lim = 50u * (SYST_RVR + 1u);
    while ((txRb->rd != txRb->wr || txRb_realtime->rd != txRb_realtime->wr) && DWT_CYCCNT - t0 < lim) {}
    speed = bb_midi_set_speed(bps, 50);
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
    TRIGGER_SW_IRQ2();                                             /* always: the bytes must move in and out even while MCL holds its MIDI lock (it does in its
                                                                      blocking sysex waits: peering, kit / pattern requests); only the processing respects it */
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
    midi_rx_pump();                                                /* (what the UART's interrupt does on MCL's own hardware: no lock) */
    if (!MidiUartParent::handle_midi_lock)
        handleIncomingMidi();                                      /* (while MCL holds the lock, its own task processes the input: handleIncomingMidi() in its wait loops) */
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
    if (cable_armed && MidiClock.state == MidiClockClass::STARTED && MidiClock.mod6_counter == 0 && MidiClock.div16th_counter == cable_step) {
        cable_armed = 0;                                           /* the RAM record's first step: the file plays into the Machinedrum's input from here */
        bb_player_play();
    }
    if (MidiClock.state == MidiClockClass::STARTED && !MidiClock.inCallback)
        TRIGGER_SW_IRQ1();
}

/* MIDI between MCL and the jack (diagnostics and tests): 0 bytes into MCL, 1 clock / transport bytes from the jack dropped, 2 bytes out, 3 clock / transport bytes of MCL's output dropped,
   4 / 5 bytes into MCL's port 2 (USB host, Trigger2MIDI INT) / clock and transport bytes of those dropped */
HOOK uint32_t bb_mcl_midi_stat(int which)
{
    switch (which) {
    case 0: return st_rx_in;
    case 1: return st_rx_clk;
    case 2: return st_tx_out;
    case 4: return st_ext_in;
    case 5: return st_ext_clk;
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

/* a byte into a side channel of MCL's output, the way MCLSeq::seq() hands its notes over (tests) */
static uint8_t side_buf[64];
static RingBuffer<> side_ring(side_buf, sizeof side_buf);
HOOK void bb_mcl_midi_put_side(uint32_t b)
{
    side_ring.put_h_isr((uint8_t)b);
    MidiUart.txRb_sidechannel = &side_ring;
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

/* The Blackbox is the clock master here (bb_clock sends the exact clock and transport to the jack). When MCL connects a Machinedrum it writes the MD's sync
 * settings from its own clock source setting: with the default (port 1 = the MD) the MD would send clock and ignore the clock it gets, so it would run at its
 * own tempo. After every pass of MCL's loop: if a connected MD is not set to receive clock (or is set to send it), it is set to clock in, no clock out
 * (transport in / out as MCL set them). MCL's own setting and config.mcls stay as they are. */
static void md_follow_blackbox(void)
{
    if (!MD.connected || (MD.global.clockIn && !MD.global.clockOut))
        return;
    MD.global.clockIn = true;
    MD.global.clockOut = false;
    MD.setExternalSync();
}

/* ---- the SAMPLE button (cable mode): MCL's sample browser, a file played into the Machinedrum's input A from the Blackbox's Out 3 while its RAM
 * record runs (bb_player.cpp streams it, the cave's audio stage plays it). Listening while browsing is optional (SAMPLE pressed again). ---- */

/* The browser's selection settled on a file (cable mode, listening on): play it from the start. */
HOOK void bb_sample_browse(const char *name)
{
    if (!bb_cable_mode || !bb_cable_listen || cable_armed)
        return;
    if (bb_player_open(name)) {
        bb_player_pump();
        bb_player_play();
    }
}

/* YES on a file (cable mode): load it, set up the RAM record of MCL's RAM page A (track 15, input A, the length rounded up to whole bars of
 * 4 steps at the current tempo, at most 32), and play it when that record starts. The RAM page then shows (NO slices it there). */
HOOK void bb_sample_record(const char *name)
{
    if (!bb_player_open(name)) {
        oled_display.textbox("NOT A", "WAV");
        return;
    }
    bb_player_pump();
    struct bb_player *p = bb_player();
    float tempo = MidiClock.get_tempo();
    if (tempo < 30.f)
        tempo = 120.f;
    float secs = (float)p->end * (float)p->step / 65536.f / 48000.f;
    uint32_t steps = (uint32_t)(secs * tempo / 15.f) + 1u;        /* 16ths */
    uint32_t enc = (steps + 3u) / 4u;
    ram_a_param4.cur = (int)(enc < 1 ? 1 : enc > 8 ? 8 : enc);
    ram_a_param1.cur = 1;                                          /* SOURCE_INPA */
    ram_page_a.setup_ram_rec_mono(14, 64, 1);
    cable_step = ram_page_a.transition_step;
    cable_armed = 1;
    mcl.setPage(RAM_PAGE_A);
}

static const char *volatile open_req;                            /* (tests: a file for the player, opened from MCL's task, where the card works) */
static volatile int open_res = -1;
HOOK void bb_player_open_req(const char *name)
{
    open_res = -1;
    open_req = name;
}
HOOK int bb_player_open_res(void) { return open_res; }

static void cable_loop(void)
{
    if (open_req) {
        const char *nm = open_req;
        open_req = 0;
        open_res = bb_player_open(nm);
    }
    bb_player_pump();
    if (bb_cable_mode && mcl.currentPage() != SAMPLE_BROWSER) {   /* left the browser: listening ends (an armed record keeps its file) */
        bb_cable_mode = 0;
        if (!cable_armed)
            bb_player_close();
    }
    if (cable_armed && grid_page.active_slots[14] != SLOT_RAM_RECORD && grid_page.active_slots[14] != SLOT_PENDING) {
        cable_armed = 0;                                           /* (the record was replaced before it began) */
    }
}

/* ---- the page's shortcuts (the Blackbox's GUI task asks, MCL's task opens the page between two passes of its loop: MCL's pages are not
 * thread safe). 0 chromatic (MCL's pitch sequencer; again: its POLY voice selection), 1 the Machinedrum's delay, 2 its reverb (again: the
 * FX page's other half, FX A <-> FX B, as MCL's own key does), 3 the mixer, 4 the grid. ---- */
static const PageIndex shortcut_page[6] = {SEQ_PTC_PAGE, FX_PAGE_A, FX_PAGE_B, MIXER_PAGE, GRID_PAGE, SAMPLE_BROWSER};
static volatile uint8_t shortcut_want = 0xff;

HOOK void bb_mcl_shortcut(uint32_t n)
{
    if (n < 6)
        shortcut_want = (uint8_t)n;
}

HOOK uint32_t bb_mcl_shortcut_now(void)
{
    PageIndex p = mcl.currentPage();
    if (p == POLY_PAGE)
        return 0;
    if (p == SAMPLE_BROWSER && !bb_cable_mode)
        return 255;                                                /* (MCL's own sample manager, not the SAMPLE button's browser) */
    for (uint32_t i = 0; i < 6; i++)
        if (p == shortcut_page[i])
            return i;
    return 255;
}

/* the grid page as data (abi 6): 0 unless MCL shows its grid page (and not its slot menu, which draws over it) */
HOOK uint32_t bb_mcl_grid(struct bb_grid *g)
{
    if (mcl.currentPage() != GRID_PAGE || grid_page.show_slot_menu || grid_page.bank_popup)
        return 0;
    GridPage &p = grid_page;
    uint32_t col = p.getCol(), row = p.getRow();
    g->cur_col = (uint8_t)(col < BB_GRID_TRACKS ? col : BB_GRID_TRACKS - 1);
    g->cur_row = p.cur_row < BB_GRID_ROWS ? p.cur_row : BB_GRID_ROWS - 1;
    g->sel_w = (uint8_t)(param3.cur > 0 ? param3.cur : 1);
    g->sel_h = (uint8_t)(param4.cur > 0 ? param4.cur : 1);
    g->grid = p.cur_grid;
    g->state = (uint8_t)MidiClock.state;
    g->base_row = (uint16_t)(row - g->cur_row);
    float t = MidiClock.get_tempo();
    g->tempo10 = (uint16_t)(t > 0.f && t < 999.f ? t * 10.f + .5f : 0.f);
    g->bar = (uint8_t)MidiClock.bar_counter;
    g->beat = (uint8_t)MidiClock.beat_counter;
    for (uint32_t x = 0; x < BB_GRID_TRACKS; x++) {
        GridRow a = p.active_slots[x + GRID_WIDTH * p.cur_grid];
        g->active[x] = a < GRID_LENGTH ? (uint8_t)a : 0xffu;
    }
    for (uint32_t y = 0; y < BB_GRID_ROWS; y++) {
        for (uint32_t x = 0; x < BB_GRID_TRACKS; x++) {
            g->label[y][x][0] = p.slot_labels[y][x][0];
            g->label[y][x][1] = p.slot_labels[y][x][1];
        }
        for (uint32_t k = 0; k < 16; k++)
            g->rowname[y][k] = p.row_headers[y].name[k];
        g->rowname[y][16] = 0;
    }
    MidiDevice *d[2] = {device_manager.primary_device(), device_manager.secondary_device()};
    for (int i = 0; i < 2; i++) {
        const char *n = d[i] ? d[i]->name : "";
        g->dev[i][0] = n[0] ? n[0] : ' ';
        g->dev[i][1] = n[0] && n[1] ? n[1] : ' ';
        g->dev[i][2] = 0;
    }
    return 1;
}

/* the mixer page as data (abi 6): 0 unless MCL shows its mixer's faders for the Machinedrum (not its menu, a perf preview or the knobs' popup) */
HOOK uint32_t bb_mcl_mixer(struct bb_mixer *m)
{
    MixerPage &p = mixer_page;
    if (mcl.currentPage() != MIXER_PAGE || p.show_mixer_menu || p.preview_mute_set != 255 || p.fill_edit_mode || !p.mixer_target.is_md_device())
        return 0;
    uint8_t n = p.mixer_track_count();
    m->param = p.display_mode;
    m->count = n > 16 ? 16 : n;
    m->held = m->muted = 0;
    uint8_t *lv = p.mixer_meter_levels();
    int first = -1;
    for (uint8_t i = 0; i < 16; i++) {
        MidiDeviceMixerParam info;
        m->value[i] = i < m->count && p.mixer_target.param(i, p.display_mode, &info) ? p.mixer_target.param_value_7bit(info) : 0;
        m->meter[i] = i < m->count && lv ? lv[i] : 0;
        if (i < m->count && note_interface.is_note_on(i)) {
            m->held |= (uint16_t)(1u << i);
            if (first < 0)
                first = i;
        }
        SeqTrack *t = i < m->count ? p.mixer_seq_track(i) : nullptr;
        if (t && t->mute_state != SEQ_MUTE_OFF)
            m->muted |= (uint16_t)(1u << i);
    }
    const char *nm = model_param_name(MD.kit.get_model(first < 0 ? 0 : first), p.display_mode);
    uint32_t k = 0;
    for (; nm && nm[k] && k < 7; k++)
        m->pname[k] = nm[k];
    m->pname[k] = 0;
    m->roll = p.beat_repeat_armed;
    beat_repeat_rate_name(mcl_cfg.beat_repeat_rate, m->roll_rate, sizeof m->roll_rate);
    return 1;
}

static volatile uint8_t page_req = 0xff;
HOOK void bb_mcl_page_req(uint32_t page)                           /* (tests: open any of MCL's pages, from its task like a shortcut) */
{
    page_req = (uint8_t)page;
}

static void shortcut_apply(void)
{
    if (page_req != 0xff) {
        PageIndex pg = (PageIndex)page_req;
        page_req = 0xff;
        mcl.setPage(pg);
    }
    uint8_t n = shortcut_want;
    if (n >= 6)
        return;
    shortcut_want = 0xff;
    PageIndex now = mcl.currentPage(), want = shortcut_page[n];
    if (n == 0 && now == SEQ_PTC_PAGE) {                         /* chromatic again: the poly voice selection (MCL's own way in: B1 + B4) */
        seq_ptc_page.re_init = true;
        mcl.pushPage(POLY_PAGE);
    } else if ((n == 1 || n == 2) && now == want) {              /* delay / reverb again: FX A <-> FX B (MCL's toggle_mode) */
        FXPage &fx = n == 1 ? fx_page_a : fx_page_b;
        fx.page_mode = !fx.page_mode;
        fx.update_encoders();
    } else if (n == 5) {                                         /* sample: MCL's sample browser in cable mode; pressed again there, listening on / off */
        if (now == SAMPLE_BROWSER && bb_cable_mode) {
            bb_cable_listen = !bb_cable_listen;
            if (!bb_cable_listen && !cable_armed)
                bb_player_stop();
        } else {
            bb_cable_mode = 1;
            mcl.setPage(SAMPLE_BROWSER);
        }
    } else if (now != want) {
        mcl.setPage(want);
    }
}

/* ---- the task ---- */
static void mcl_task(void *)
{
    {
        uint32_t *c = (uint32_t *)*(volatile uint32_t *)0x2401f2a8u;     /* (diagnostics: the kernel's view of this task) */
        bb_dbg[17] = (uint32_t)c;
        bb_dbg[18] = c[4] == (uint32_t)c;
        uint32_t sp;
        __asm volatile("mov %0, sp" : "=r"(sp));
        bb_dbg[28] = sp;
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
        md_follow_blackbox();
        shortcut_apply();
        cable_loop();
        if (!bb_midi_owner())
            bb_midi_claim(1);                                      /* (still waiting for an idle moment of the stock transmitter) */
        bb_sleep_ms(1);
    }
}

/* MCL's task lives in our own SDRAM, not in the stock kernel's heap: that heap is 36 KB and the stock tasks' stacks already take about 34 KB of it,
 * yet the kernel handed out 16 KB for MCL's stack and its TCB was wiped within a millisecond on the unit (docs/mcl-m0-m1-results.md, M8). */
#define MCL_STACK_WORDS 4096u                                       /* 16 KB (MCL needs about 13 KB) */
extern "C" {
__attribute__((aligned(8))) uint32_t bb_mcl_tcb[0x60 / 4];          /* the stock kernel's TCB is 0x54 bytes (its xTaskCreate allocates that) */
__attribute__((aligned(8))) uint32_t bb_mcl_stack[MCL_STACK_WORDS];
}

/* The stock xTaskCreate (0x08088ad8) without its two pvPortMalloc calls: the same register set-up its prologue makes (r4 TCB, r5 stack bytes, r6 stack,
 * r7 name, r8 function, r9 argument, r0 TCB), then on into its body at 0x08088b00, which fills the TCB, builds the first stack frame, adds the task to the
 * ready list and returns pdPASS (1) through the pop of this push. Arguments as xTaskCreate's: (function, name, stack words, argument, priority, &handle). */
__attribute__((naked, noinline)) static uint32_t create_in_place(void (*)(void *), const char *, uint32_t, void *, uint32_t, uint32_t *)
{
    __asm volatile(
        "push.w {r4, r5, r6, r7, r8, r9, r10, lr}\n"
        "mov r8, r0\n"
        "mov r7, r1\n"
        "mov r9, r3\n"
        "lsls r5, r2, #2\n"
        "ldr r6, =bb_mcl_stack\n"
        "ldr r4, =bb_mcl_tcb\n"
        "mov r0, r4\n"
        "ldr r12, =0x08088b01\n"
        "bx r12\n"
        ".ltorg\n");
}

/* Creates the task (from the UI task, once, when the runtime is up), at osPriorityNormal (kernel priority 3) like the UI and the streamer tasks.
 * Returns the task handle (its TCB), 0 if the kernel refused. */
HOOK uint32_t bb_mcl_start(uint32_t stack_words)
{
    if (mcl_state)
        return 1;
    if (stack_words > MCL_STACK_WORDS || !stack_words)
        stack_words = MCL_STACK_WORDS;
    uint32_t h = 0;
    mcl_state = 1;                                                  /* (before the task exists: the caller must not get here twice) */
    if (create_in_place(mcl_task, "MCL", stack_words, nullptr, 3u, &h) != 1u || !h)
        mcl_state = 3;
    else {
        mcl_stack = stack_words;
        bb_dbg[13] = h;                                             /* (diagnostics) */
        bb_dbg[16] = ((volatile uint32_t *)h)[4] == h;
        bb_dbg[26] = ((volatile uint32_t *)h)[12];                  /* the stack base (+0x30) and saved top of stack (word 0) as the kernel built them */
        bb_dbg[27] = ((volatile uint32_t *)h)[0];
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
