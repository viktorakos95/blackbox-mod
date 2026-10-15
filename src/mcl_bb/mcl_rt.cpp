/*
 * MCL runtime for the Blackbox (milestone M2): what a C++ program needs before main(), in a code blob that runs in place from flash.
 *
 * boot() (called once per power-up by cpu.c start(); the caller must have switched the backup SRAM on, bkp_enable()):
 *   1. checks the SDRAM region this blob's .data/.bss/heap are linked to is really reserved (patches/mclblob.py lowers the allocator's
 *      initial top in the stock .data image to RAM_ORG; without that patch the region belongs to the stock allocator and nothing is touched),
 *   2. copies .data from flash, zeroes .bss, gives the heap its range, runs the static constructors,
 *   3. runs a tiny self test (constructor, data, bss, heap, virtual call) and records the result.
 * Result: backup SRAM 0x38800920 (magic, code, constructors run, bss bytes), text "Mok" / "M<code>" for the version label.
 * Codes: 1 ok, 2 region not reserved, 3 self test failed (bit n of the second status word = test n), 4 region too small.
 */
#include <stddef.h>
#include <stdint.h>

#include "bb_clock.h"
#include "bb_m3test.h"
#include "bb_midi.h"
#include "bb_sd.h"
#include "bb_ui.h"

/* When MCL itself is linked in (bb_mcl.cpp: the strong definitions) the page shows MCL's screen and feeds its panel; otherwise the diagnostic test bench (bb_ui.cpp). */
extern "C" {
__attribute__((weak)) int bb_mcl_active(void) { return 0; }
__attribute__((weak)) const uint8_t *bb_mcl_fb(void) { return 0; }
__attribute__((weak)) uint32_t bb_mcl_frames(void) { return 0; }
__attribute__((weak)) uint32_t bb_mcl_start(uint32_t) { return 1; }
__attribute__((weak)) void bb_sleep_poke(void) {}
__attribute__((weak)) void bb_mcl_page_input(uint32_t, int32_t, int32_t) {}
}

extern "C" {
extern uint32_t __data_lma[], __data_start[], __data_end[], __bss_start[], __bss_end[], __heap_start[], __heap_limit[];
typedef void (*ctor_t)(void);
extern ctor_t __init_array_start[], __init_array_end[];
}

#define RAM_ORG      0xC3F80000u
#define ALLOC_TOP    (*(volatile uint32_t *)0x24000088u)       /* stock SDRAM allocator: top (moves down) */
#define ALLOC_TOP_INIT (*(volatile uint32_t *)0x080f1e08u)     /* ... and its initial value in the flash .data image */
#define BKP          ((volatile uint32_t *)0x38800920u)
#define BKP_MAGIC    0x4d434c31u                               /* "MCL1" */

enum { ST_OK = 1, ST_NO_REGION = 2, ST_SELFTEST = 3, ST_TOO_SMALL = 4 };

/* ---- heap: first fit, address-ordered free list, coalescing; 16-byte aligned, 16-byte header ---- */
struct blk { uint32_t size; uint32_t used; blk *next; uint32_t pad; };
static blk *free_list;
static uint32_t heap_bytes_total, heap_in_use;

static void heap_init(void)
{
    uintptr_t a = ((uintptr_t)__heap_start + 15u) & ~15u;
    blk *b = (blk *)a;
    b->size = (uint32_t)((uintptr_t)__heap_limit - a) - sizeof(blk);
    b->used = 0;
    b->next = 0;
    free_list = b;
    heap_bytes_total = b->size;
    heap_in_use = 0;
}

extern "C" void *malloc(size_t n)
{
    n = (n + 15u) & ~15u;
    if (!n)
        n = 16;
    for (blk **pp = &free_list; *pp; pp = &(*pp)->next) {
        blk *b = *pp;
        if (b->size < n)
            continue;
        if (b->size >= n + sizeof(blk) + 16) {                     /* split */
            blk *r = (blk *)((uint8_t *)(b + 1) + n);
            r->size = b->size - n - (uint32_t)sizeof(blk);
            r->used = 0;
            r->next = b->next;
            *pp = r;
            b->size = (uint32_t)n;
        } else {
            *pp = b->next;
        }
        b->used = 1;
        b->next = 0;
        heap_in_use += b->size;
        return b + 1;
    }
    return 0;
}

extern "C" void free(void *p)
{
    if (!p)
        return;
    blk *b = (blk *)p - 1;
    if (!b->used)
        return;
    b->used = 0;
    heap_in_use -= b->size;
    blk **pp = &free_list;
    while (*pp && *pp < b)
        pp = &(*pp)->next;
    b->next = *pp;
    *pp = b;
    if (b->next && (uint8_t *)(b + 1) + b->size == (uint8_t *)b->next) {          /* merge with the next block */
        b->size += (uint32_t)sizeof(blk) + b->next->size;
        b->next = b->next->next;
    }
    if (pp != &free_list) {                                                        /* ... and with the previous one */
        blk *prev = (blk *)((uint8_t *)pp - offsetof(blk, next));
        if ((uint8_t *)(prev + 1) + prev->size == (uint8_t *)b) {
            prev->size += (uint32_t)sizeof(blk) + b->size;
            prev->next = b->next;
        }
    }
}

void *operator new(size_t n) { return malloc(n); }
void *operator new[](size_t n) { return malloc(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

/* ---- what the compiler and libgcc ask for ---- */
extern "C" {
void *__dso_handle;
int __cxa_atexit(void (*)(void *), void *, void *) { return 0; }       /* nothing ever exits */
int __aeabi_atexit(void *, void (*)(void *), void *) { return 0; }
void __cxa_pure_virtual(void) { for (;;) {} }
int __cxa_guard_acquire(uint64_t *g) { return !*(char *)g; }
void __cxa_guard_release(uint64_t *g) { *(char *)g = 1; }
void __cxa_guard_abort(uint64_t *) {}

void *memset(void *d, int c, size_t n)
{
    uint8_t *p = (uint8_t *)d;
    while (n--)
        *p++ = (uint8_t)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *p = (uint8_t *)d;
    const uint8_t *q = (const uint8_t *)s;
    while (n--)
        *p++ = *q++;
    return d;
}
}

/* ---- M2 self test: the things MCL's 39 constructors, its globals and its two malloc users depend on ---- */
struct Base {
    virtual int f() { return 1; }
};
struct Probe : Base {
    int made;
    Probe() : made(0x5a5a) { ctor_runs++; }
    int f() override { return 2; }
    static int ctor_runs;
};
int Probe::ctor_runs;
static Probe probe;                                   /* needs a constructor and a vtable */
static volatile uint32_t data_probe[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};     /* .data */
static volatile uint8_t bss_probe[2048];                                                           /* .bss */

static uint32_t selftest(void)
{
    uint32_t bad = 0;
    if (probe.made != 0x5a5a || Probe::ctor_runs != 1)
        bad |= 1;
    Base *b = &probe;
    if (b->f() != 2)
        bad |= 2;
    if (data_probe[0] != 0x11111111u || data_probe[3] != 0x44444444u)
        bad |= 4;
    for (unsigned i = 0; i < sizeof bss_probe; i++)
        if (bss_probe[i])
            bad |= 8;
    void *p1 = malloc(100), *p2 = malloc(5000), *p3 = malloc(16);
    if (!p1 || !p2 || !p3 || (((uintptr_t)p1 | (uintptr_t)p2 | (uintptr_t)p3) & 15u))
        bad |= 16;
    free(p2);
    free(p1);
    free(p3);
    void *big = malloc(heap_bytes_total - 64);          /* everything merged back into one block? */
    if (!big)
        bad |= 32;
    free(big);
    if (heap_in_use)
        bad |= 64;
    return bad;
}

extern "C" uint32_t mcl_boot(void)
{
    volatile uint32_t *s = BKP;
    bb_midi_down();                                      /* hooks off before .bss is touched */
    bb_clock_down();
    bb_sd_down();
    s[0] = BKP_MAGIC;
    s[2] = s[3] = 0;
    if (ALLOC_TOP_INIT != RAM_ORG || ALLOC_TOP > RAM_ORG) {
        s[1] = ST_NO_REGION;
        return ST_NO_REGION;
    }
    if ((uintptr_t)__bss_end > (uintptr_t)__heap_limit - 4096u) {
        s[1] = ST_TOO_SMALL;
        return ST_TOO_SMALL;
    }
    for (uint32_t *d = __data_start, *l = __data_lma; d < __data_end;)
        *d++ = *l++;
    for (uint32_t *d = __bss_start; d < __bss_end;)
        *d++ = 0;
    heap_init();
    uint32_t n = 0;
    for (ctor_t *c = __init_array_start; c < __init_array_end; c++, n++)
        (*c)();
    uint32_t bad = selftest();
    if (!bad) {
        bb_midi_init();                                  /* the MIDI hooks go live last */
        bb_sd_init();                                    /* (the streamer's hook only runs requests once this has set its ready flag) */
    }
    s[2] = n;
    s[3] = (uint32_t)((uintptr_t)__bss_end - (uintptr_t)__bss_start);
    uint32_t st = bad ? ST_SELFTEST : ST_OK;
    s[1] = st | (bad << 8);
    return st;
}

#define DWT_CYCCNT (*(volatile uint32_t *)0xe0001004u)
#ifdef M3_TEST
static uint8_t m3_ran;
#endif

#ifdef M5_TEST
static uint8_t m5_ran;
static uint32_t m5_s, m5_b, m5_e;                   /* sync state, tempo (1/10 BPM), largest phase error of the last second (us) after the measurement */
#endif

#ifdef M4_TEST
static uint8_t m4_ran;
static uint32_t m4_t, m4_r, m4_l;                   /* loop-back frame result (1 = back, 2 = not, 3 = no claim); after 250 ms of the clock: ticks, bytes received from the jack */
#endif

/* Runs the blocking M3 hardware test once, from a label constructor (GUI task, never the audio task). No-op in normal images. */
extern "C" void bb_rt_gui(void)
{
    {
        volatile uint32_t *sm = BKP;
        static uint8_t mcl_started;
        if (!mcl_started && sm[0] == BKP_MAGIC && (sm[1] & 0xff) == ST_OK) {
            mcl_started = 1;
            bb_mcl_start(4096);                                  /* MCL's own task (a no-op in builds without MCL) */
        }
    }
#ifdef M3_TEST
    volatile uint32_t *s = BKP;
    if (m3_ran || s[0] != BKP_MAGIC || (s[1] & 0xff) != ST_OK)
        return;
    m3_ran = 1;
    uint32_t c0 = DWT_CYCCNT;
    bb_m3_reset();
    while (!bb_m3_step((DWT_CYCCNT - c0) / 400u))      /* ~400 MHz core: cycles / 400 = microseconds */
        if ((DWT_CYCCNT - c0) / 400u > 4000000u)
            break;
#endif
#ifdef M5_TEST
    /* M5 hardware test image: the clock follows the MIDI clock arriving on the jack (a master running before power-up). Up to 5 s to lock, 2 s of measurement. */
    volatile uint32_t *s5 = BKP;
    if (!m5_ran && s5[0] == BKP_MAGIC && (s5[1] & 0xff) == ST_OK) {
        m5_ran = 1;
        bb_midi_claim(1);
        if (bb_midi_owner() && bb_clock_init()) {
            bb_midi_filter_clock(1);
            bb_clock_sync_midi();
            uint32_t c0 = DWT_CYCCNT;
            while (bb_clock_sync_state() != 3 && DWT_CYCCNT - c0 < 2000000000u) {}     /* 5 s at 400 MHz */
            c0 = DWT_CYCCNT;
            while (DWT_CYCCNT - c0 < 800000000u) {}                                    /* 2 s of locked clock */
            m5_s = (uint32_t)bb_clock_sync_state();
            m5_b = bb_clock_bpm_x10();
            m5_e = bb_clock_err_us(0);
        }
    }
#endif
#ifdef M4_TEST
    /* M4 hardware test image: from the first label on, MIDI clock at 120.0 BPM from the timer interrupt, on the jack, until power-off. No start / stop bytes. */
    volatile uint32_t *s4 = BKP;
    if (!m4_ran && s4[0] == BKP_MAGIC && (s4[1] & 0xff) == ST_OK) {
        m4_ran = 1;
        bb_midi_claim(1);
        if (bb_midi_owner()) {
            /* 1. the cable check, with the code that already worked on the unit: one 103-byte frame out and back (150 ms time-out) */
            uint32_t c0 = DWT_CYCCNT;
            bb_m3_reset();
            bb_m3_loop_only(1);
            while (!bb_m3_step((DWT_CYCCNT - c0) / 400u))
                if ((DWT_CYCCNT - c0) / 400u > 1000000u)
                    break;
            m4_l = bb_m3_result()->loop;
            /* 2. the clock for 250 ms: ~96 ticks and 12 clock bytes expected, and with a closed loop the same 12 coming back */
            uint32_t r0 = bb_midi_stat(0);
            if (bb_clock_init()) {
                bb_midi_filter_clock(1);
                bb_clock_start(1200, 0);
                c0 = DWT_CYCCNT;
                while (DWT_CYCCNT - c0 < 100000000u) {}       /* 250 ms at 400 MHz */
                m4_t = bb_clock_ticks();
                m4_r = bb_midi_stat(0) - r0;
            }
        }
    }
#endif
}

/* "Mok" or "M<code>" (+ failed test bits as hex), then the M3 test result if this image ran it. Writes at most `room` characters;
 * returns the end of the text. */
extern "C" char *mcl_status_text(char *p, uint32_t room)
{
    char t[24];
    char *q = t;
    volatile uint32_t *s = BKP;
    *q++ = 'M';
    if (s[0] != BKP_MAGIC) {
        *q++ = '-';
    } else {
        uint32_t st = s[1] & 0xff, bad = s[1] >> 8;
        if (st == ST_OK) {
            *q++ = 'o';
            *q++ = 'k';
        } else {
            *q++ = (char)('0' + st);
            if (bad) {
                *q++ = "0123456789abcdef"[(bad >> 4) & 15];
                *q++ = "0123456789abcdef"[bad & 15];
            }
        }
    }
#ifdef M3_TEST
    const m3_result *r = bb_m3_result();
    if (r->loop || r->md) {
        *q++ = ' ';
        *q++ = 'L';
        *q++ = r->loop == 1 ? '+' : r->loop == 3 ? '?' : '-';
        *q++ = ' ';
        *q++ = 'D';
        if (r->md == 1) {
            *q++ = (char)('0' + r->kit / 10);
            *q++ = (char)('0' + r->kit % 10);
            *q++ = ' ';
            *q++ = 'K';
            *q++ = r->dump == 1 ? '+' : '-';
        } else {
            *q++ = r->md == 3 ? '?' : '-';
            if (r->burst) {                           /* the note burst was played: listen to the Machinedrum */
                *q++ = ' ';
                *q++ = 'N';
            }
        }
    }
#endif
    if (s[0] != BKP_MAGIC || (s[1] & 0xff) != ST_OK) {        /* no booted runtime: the clock variables in SDRAM are garbage */
    } else if (bb_clock_running() || bb_clock_sync_state()) {      /* the timer clock is running (M4 image: 120.0 BPM, fixed), or listening for a master (M5 image) */
#if defined(M5_TEST)
        /* ' S3 1200 e9': sync state (3 = locked and sending, 1 = never saw a clock), tempo in 1/10 BPM, largest phase error of the last second in microseconds */
        char *w = q;
        *w++ = ' ';
        *w++ = 'S';
        *w++ = (char)('0' + m5_s % 10);
        *w++ = ' ';
        for (uint32_t d = 1000; d; d /= 10)
            if (m5_b >= d || d == 1)
                *w++ = (char)('0' + m5_b / d % 10);
        *w++ = ' ';
        *w++ = 'e';
        uint32_t n = m5_e > 999 ? 999 : m5_e;
        if (n >= 100)
            *w++ = (char)('0' + n / 100);
        if (n >= 10)
            *w++ = (char)('0' + n / 10 % 10);
        *w++ = (char)('0' + n % 10);
        q = w;
#elif defined(M4_TEST)
        /* ' L+ t96 r12': the loop-back frame came back (L+; L- no, L? port not claimed), then after 250 ms of the clock: ticks and the clock bytes the receive tap saw on the jack. */
        char *w = q;
        *w++ = ' ';
        *w++ = 'L';
        *w++ = m4_l == 1 ? '+' : m4_l == 3 ? '?' : '-';
        const uint32_t v[2] = {m4_t, m4_r};
        const char tag[2] = {'t', 'r'};
        for (int k = 0; k < 2; k++) {
            *w++ = ' ';
            *w++ = tag[k];
            uint32_t n = v[k] > 999 ? 999 : v[k];
            if (n >= 100)
                *w++ = (char)('0' + n / 100);
            if (n >= 10)
                *w++ = (char)('0' + n / 10 % 10);
            *w++ = (char)('0' + n % 10);
        }
        q = w;
#else
        *q++ = ' ';
        *q++ = 'C';
        *q++ = '1';
        *q++ = '2';
        *q++ = '0';
#endif
    } else if (bb_clock_timer_hz()) {
        *q++ = ' ';
        *q++ = 'C';
        *q++ = '-';
    }
    for (char *c = t; c < q && room; c++, room--)
        *p++ = *c;
    return p;
}

struct mcl_entry {
    uint32_t magic;                                   /* "MCLB" */
    uint32_t abi;                                     /* 3: boot, status(p, room), gui(), compact; then the page: fb(), sig(), input() */
    uint32_t (*boot)(void);
    char *(*status)(char *, uint32_t);
    void (*gui)(void);
    uint32_t compact;                                 /* 1 in the hardware test images: the version label shows ONLY the status text (the stock text after it runs off the screen) */
    const uint8_t *(*fb)(void);                       /* the 128x32 screen (bb_ui.h); null while the runtime is not up */
    uint32_t (*sig)(void);                            /* draws it; a hash that changes with the picture (0 while the runtime is not up) */
    void (*input)(uint32_t kind, int32_t a, int32_t b);     /* encoders and buttons from the page, GUI task */
};
static int ready(void)
{
    volatile uint32_t *s = BKP;
    return s[0] == BKP_MAGIC && (s[1] & 0xff) == ST_OK;
}
/* MCL's screen once it has drawn; until then (and if it never does) the bench's diagnostic view of where its task is */
static int mcl_drawn(void) { return bb_mcl_active() && bb_mcl_frames() != 0; }
static const uint8_t *page_fb(void) { return !ready() ? 0 : mcl_drawn() ? bb_mcl_fb() : bb_ui_fb(); }
static uint32_t page_sig(void) { if (bb_mcl_active() && !mcl_drawn()) bb_sleep_poke(); return !ready() ? 0 : mcl_drawn() ? bb_mcl_frames() * 2654435761u + 1u : bb_ui_sig(); }
static void page_input(uint32_t kind, int32_t a, int32_t b)
{
    if (!ready())
        return;
    if (bb_mcl_active()) {
        if (mcl_drawn())
            bb_mcl_page_input(kind, a, b);
    } else
        bb_ui_input(kind, a, b);
}
#if defined(M3_TEST) || defined(M4_TEST) || defined(M5_TEST) || defined(COMPACT_LABEL)
#define COMPACT 1u
#else
#define COMPACT 0u
#endif
extern "C" const mcl_entry mcl_entry_table __attribute__((section(".entry"), used)) = {0x424c434du, 3, mcl_boot, mcl_status_text, bb_rt_gui, COMPACT, page_fb, page_sig, page_input};
