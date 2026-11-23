#include "bb_sdspy.h"

#define HOOK extern "C" __attribute__((section(".hook"), used))
#define PX_CURRENT_TCB (*(volatile uint32_t *)0x2401f2a8u)       /* FreeRTOS pxCurrentTCB (the PendSV handler's literal) */
#define H_DEFAULT      (*(volatile uint32_t *)0x24002e78u)       /* thread handles stored by main() (0x080442b0..): osThreadNew results */
#define H_AUDIO        (*(volatile uint32_t *)0x24002e74u)
#define H_PCM          (*(volatile uint32_t *)0x24002e70u)
#define DWT_CYCCNT     (*(volatile uint32_t *)0xe0001004u)
#define SYST_RVR       (*(volatile uint32_t *)0xe000e014u)

static volatile uint32_t calls[SPY_TASKS][SPY_FUNCS];
static volatile uint32_t disk[SPY_TASKS][SPY_DISK_FUNCS];
static volatile uint32_t depth[SPY_LAYERS][SPY_TASKS];
static volatile uint32_t overlaps[SPY_LAYERS], first_pair[SPY_LAYERS] = {0xff, 0xff};
static volatile uint32_t worst_cyc[SPY_LAYERS];
static volatile uint32_t bufcls[2][5], unaligned[2], bufmin[2] = {0xffffffffu, 0xffffffffu}, bufmax[2];

static void note_buf(int rw, uint32_t buf)
{
    uint32_t top = buf >> 24;
    int c = top == 0x20 ? 0 : top == 0x24 ? 1 : top == 0x30 ? 2 : top >= 0xc0 && top <= 0xc7 ? 3 : 4;
    bufcls[rw][c]++;
    if (buf < bufmin[rw])
        bufmin[rw] = buf;
    if (buf > bufmax[rw])
        bufmax[rw] = buf;
    if (buf & 31u)
        unaligned[rw]++;
}

static inline uint32_t irq_off(void)
{
    uint32_t p;
    __asm volatile("mrs %0, primask\n cpsid i" : "=r"(p)::"memory");
    return p;
}
static inline void irq_restore(uint32_t p) { __asm volatile("msr primask, %0" ::"r"(p) : "memory"); }

HOOK int bb_sdspy_task(void)
{
    uint32_t t = PX_CURRENT_TCB;
    return t == H_DEFAULT ? SPY_DEFAULT : t == H_AUDIO ? SPY_AUDIO : t == H_PCM ? SPY_PCM : SPY_OTHER;
}

HOOK void bb_sdspy_enter(int layer, int task)
{
    uint32_t p = irq_off();
    for (int t = 0; t < SPY_TASKS; t++)
        if (t != task && depth[layer][t]) {
            overlaps[layer]++;
            if (first_pair[layer] == 0xff)
                first_pair[layer] = ((uint32_t)task << 4) | (uint32_t)t;
            break;
        }
    depth[layer][task]++;
    irq_restore(p);
}

HOOK void bb_sdspy_leave(int layer, int task)
{
    uint32_t p = irq_off();
    if (depth[layer][task])
        depth[layer][task]--;
    irq_restore(p);
}

HOOK uint32_t bb_sdspy_buf(int rw, int cls) { return bufcls[rw][cls]; }
HOOK uint32_t bb_sdspy_unaligned(int rw) { return unaligned[rw]; }

#define SCB_CCR      (*(volatile uint32_t *)0xe000ed14u)
#define MPU_TYPE     (*(volatile uint32_t *)0xe000ed90u)
#define MPU_CTRL     (*(volatile uint32_t *)0xe000ed94u)
#define MPU_RNR      (*(volatile uint32_t *)0xe000ed98u)
#define MPU_RBAR     (*(volatile uint32_t *)0xe000ed9cu)
#define MPU_RASR     (*(volatile uint32_t *)0xe000eda0u)

HOOK uint32_t bb_sdspy_bufaddr(int rw, int which) { return which ? bufmax[rw] : (bufmin[rw] == 0xffffffffu ? 0 : bufmin[rw]); }

HOOK uint32_t bb_sdspy_region(int n, int field)
{
    uint32_t p = irq_off();
    uint32_t keep = MPU_RNR;
    MPU_RNR = (uint32_t)n;
    uint32_t rasr = MPU_RASR, rbar = MPU_RBAR & ~0x1fu;
    MPU_RNR = keep;
    irq_restore(p);
    switch (field) {
    case 0: return rasr & 1u;
    case 1: return rbar;
    case 2: return ((rasr >> 1) & 31u) + 1u;
    case 3: return rasr >> 19 & 7u;
    case 4: return rasr >> 17 & 1u;
    case 5: return rasr >> 16 & 1u;
    case 6: return rasr >> 18 & 1u;
    case 7: return rasr >> 28 & 1u;
    case 8: return rasr >> 24 & 7u;
    default: return rasr >> 8 & 255u;
    }
}

HOOK uint32_t bb_sdspy_hw(int what) { return what == 0 ? SCB_CCR : what == 1 ? MPU_TYPE : MPU_CTRL; }

HOOK uint32_t bb_sdspy_mpu(uint32_t addr, int field)
{
    uint32_t regions = (MPU_TYPE >> 8) & 0xff, best = 0, best_n = 0xffff;
    uint32_t p = irq_off();
    uint32_t keep = MPU_RNR;
    for (uint32_t n = 0; n < regions; n++) {
        MPU_RNR = n;
        uint32_t rasr = MPU_RASR, rbar = MPU_RBAR & ~0x1fu;
        if (!(rasr & 1u))
            continue;
        uint32_t size = ((rasr >> 1) & 31u) + 1u;               /* region bytes = 2^size */
        uint64_t len = 1ull << size;
        if (addr < rbar || (uint64_t)addr >= (uint64_t)rbar + len)
            continue;
        if (size >= 8) {                                        /* subregions: eight equal parts, a set SRD bit disables a part */
            uint32_t sub = (uint32_t)(((uint64_t)(addr - rbar) * 8u) / len);
            if (rasr >> (8 + sub) & 1u)
                continue;
        }
        best = rasr;
        best_n = n;
    }
    MPU_RNR = keep;
    irq_restore(p);
    switch (field) {
    case 0: return best_n != 0xffff;
    case 1: return best >> 19 & 7u;
    case 2: return best >> 17 & 1u;
    case 3: return best >> 16 & 1u;
    case 4: return best >> 18 & 1u;
    case 5: return best >> 28 & 1u;
    case 6: return best >> 24 & 7u;
    default: return best_n;
    }
}

HOOK uint32_t bb_sdspy_count(int task, int fn) { return calls[task][fn]; }
HOOK uint32_t bb_sdspy_disk(int task, int fn) { return disk[task][fn]; }
HOOK uint32_t bb_sdspy_overlaps(int layer) { return overlaps[layer]; }
HOOK uint32_t bb_sdspy_pair(int layer) { return first_pair[layer]; }
HOOK uint32_t bb_sdspy_worst_us(int layer) { return (uint32_t)((uint64_t)worst_cyc[layer] * 1000000u / ((SYST_RVR + 1u) * 1000u)); }

typedef uint32_t (*fn4)(uint32_t, uint32_t, uint32_t, uint32_t);

static uint32_t run(int layer, int task, fn4 f, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    bb_sdspy_enter(layer, task);
    uint32_t t0 = DWT_CYCCNT;
    uint32_t r = f(a, b, c, d);
    uint32_t dt = DWT_CYCCNT - t0;
    if (dt > worst_cyc[layer])
        worst_cyc[layer] = dt;
    bb_sdspy_leave(layer, task);
    return r;
}

#define FAT(name, fn, addr)                                                                                      \
    HOOK uint32_t name(uint32_t a, uint32_t b, uint32_t c, uint32_t d)                                         \
    {                                                                                                            \
        int t = bb_sdspy_task();                                                                                 \
        calls[t][fn]++;                                                                                          \
        return run(SPY_FAT, t, (fn4)(addr | 1u), a, b, c, d);                                                 \
    }
/* the stock FatFs (docs/mcl-m0-m1-results.md, M0) */
FAT(spy_f_open, SPY_OPEN, 0x08086534u)
FAT(spy_f_read, SPY_READ, 0x08086866u)
FAT(spy_f_write, SPY_WRITE, 0x08086a06u)
FAT(spy_f_close, SPY_CLOSE, 0x08086d76u)
FAT(spy_f_lseek, SPY_SEEK, 0x08086d96u)
FAT(spy_f_sync, SPY_SYNC, 0x08086c28u)
FAT(spy_f_mount, SPY_MOUNT, 0x080864e8u)

/* the SD card driver's table entries (0x080cfc1c: init, status, read, write, ioctl): stock functions 0x08045974 / 0x080459e8 / 0x08045a5c */
#define DISK(name, fn, addr)                                                                                     \
    HOOK uint32_t name(uint32_t a, uint32_t b, uint32_t c, uint32_t d)                                         \
    {                                                                                                            \
        int t = bb_sdspy_task();                                                                                 \
        disk[t][fn]++;                                                                                           \
        if (fn != SPY_DISK_IO)                                                                                   \
            note_buf(fn == SPY_DISK_WR, b);                                                                      \
        return run(SPY_DISK, t, (fn4)(addr | 1u), a, b, c, d);                                                \
    }
DISK(spy_disk_read, SPY_DISK_RD, 0x08045974u)
DISK(spy_disk_write, SPY_DISK_WR, 0x080459e8u)
DISK(spy_disk_ioctl, SPY_DISK_IO, 0x08045a5cu)
