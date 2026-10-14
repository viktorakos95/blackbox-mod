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
    if (!bad)
        bb_midi_init();                                  /* the MIDI hooks go live last */
    s[2] = n;
    s[3] = (uint32_t)((uintptr_t)__bss_end - (uintptr_t)__bss_start);
    uint32_t st = bad ? ST_SELFTEST : ST_OK;
    s[1] = st | (bad << 8);
    return st;
}

#ifdef M3_TEST
#define DWT_CYCCNT (*(volatile uint32_t *)0xe0001004u)
static uint8_t m3_ran;
#endif

#ifdef M4_TEST
static uint8_t m4_ran;
#endif

/* Runs the blocking M3 hardware test once, from a label constructor (GUI task, never the audio task). No-op in normal images. */
extern "C" void mcl_gui(void)
{
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
#ifdef M4_TEST
    /* M4 hardware test image: from the first label on, MIDI clock at 120.0 BPM from the timer interrupt, on the jack, until power-off. No start / stop bytes. */
    volatile uint32_t *s4 = BKP;
    if (!m4_ran && s4[0] == BKP_MAGIC && (s4[1] & 0xff) == ST_OK) {
        m4_ran = 1;
        bb_midi_claim(1);
        if (bb_midi_owner() && bb_clock_init()) {
            bb_midi_filter_clock(1);
            bb_clock_start(1200, 0);
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
    } else if (bb_clock_running()) {                      /* ' C120': the timer clock is running, tempo is fixed in this image */
        *q++ = ' ';
        *q++ = 'C';
        *q++ = '1';
        *q++ = '2';
        *q++ = '0';
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
    uint32_t abi;                                     /* 2: status(p, room), gui() */
    uint32_t (*boot)(void);
    char *(*status)(char *, uint32_t);
    void (*gui)(void);
};
extern "C" const mcl_entry mcl_entry_table __attribute__((section(".entry"), used)) = {0x424c434du, 2, mcl_boot, mcl_status_text, mcl_gui};
