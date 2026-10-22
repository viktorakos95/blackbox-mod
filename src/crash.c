/* The crash recorder. Why did the unit restart?
 * - A fault (hard, memory, bus, usage): the fault vectors point here (patches/crash.py; the stock handlers spin until the
 *   watchdog resets). crash_fault (crash_thunk.S) hands over the stacked registers; they are written to the backup SRAM
 *   with the fault status registers and the breadcrumbs, and the unit is reset at once.
 * - The stock allocator running out (it halts, then the watchdog resets): its halt is hooked (crash_oom_thunk), the size
 *   asked for and the caller are recorded, then the same reset.
 * - Anything else (a hang: the watchdog; a power cut): no code runs, but the breadcrumbs, the uptime and the free-RAM low
 *   point of that run are still in the live block, and the reset flags say which reset it was.
 * At the next boot (crash_boot, from looper_boot) that run's record is put together; the Looper's SETUP page shows it
 * (tap the right end of its last row). The record stays until the next restart or CLEAR. */
#include "crash.h"

unsigned lira_free_kb(void);
unsigned looper_load_avg(void);

#define RCC_RSR (*(volatile uint32_t *)0x580244d0u)
#define SCB_CFSR (*(volatile uint32_t *)0xe000ed28u)
#define SCB_HFSR (*(volatile uint32_t *)0xe000ed2cu)
#define SCB_MMFAR (*(volatile uint32_t *)0xe000ed34u)
#define SCB_BFAR (*(volatile uint32_t *)0xe000ed38u)
#define SCB_AIRCR (*(volatile uint32_t *)0xe000ed0cu)

/* Backup SRAM 0x38800b20..0x38800bc0 (between Munchi's words at 0xb10..0xb1b and the DRONE flight recorder at 0xbc0). */
struct live {
    uint32_t magic;
    uint8_t bc[4];                 /* = CRASH_BC */
    uint32_t blocks;               /* audio blocks since boot */
    uint16_t kbmin, kbnow;         /* free SDRAM (KB): the lowest seen, the last */
    uint16_t load, peak;           /* the averaged whole-task load (per mille), and its highest this run */
};
struct rec {
    uint32_t magic;
    uint16_t boots, faults;
    uint32_t rsr;                  /* the reset flags of the restart this record ends with */
    uint8_t kind, state, bc[3];    /* kind: 0 none (see rsr), 1 hard, 2 memory, 3 bus, 4 usage fault, 5 out of memory; state 1 written by a handler */
    uint8_t pad_;
    uint32_t pc, lr, xpsr, sp, exc;
    uint32_t cfsr, hfsr, mmfar, bfar;
    uint32_t r0, r12;
    uint32_t stk[4];               /* the words above the exception frame (often the callers' return addresses) */
    uint32_t blocks;
    uint16_t kbmin, kbnow, load, peak;
    uint32_t oom;                  /* out of memory: the size asked for */
};
/* The stock watchdog: the audio block (0x0804cb1c) calls a check (0x08046994) that refreshes the window watchdog only while
 * two other loops have checked in within 3749 blocks (20 s): stamp A (several places, 0x080469c4) and stamp B (the GUI's
 * update, 0x080469ca). The window watchdog resets ~0.3 s after the last refresh. Those three calls come here first, so a
 * watchdog restart says which side stopped: the audio blocks (then the GUI and the other loop went on a while after them),
 * or one of the two loops (its lag grew towards 3749). */
struct wdog {
    uint32_t cyc_au, cyc_gui, cyc_a, site_a, per;  /* DWT cycle count at the last audio check / GUI stamp / A stamp; A's call site; cycles a block */
    uint16_t lag_a, lag_b;                         /* blocks since stamp A / B, at the last check */
    int16_t r_gui, r_a;                            /* at boot: ms the GUI / loop A went on after the last audio block */
    uint16_t r_lag_a, r_lag_b;
    uint32_t r_site;
};
#define WD ((volatile struct wdog *)0x38800b98u)
#define CYC (*(volatile uint32_t *)0xe0001004u)
_Static_assert(0x38800b98u + sizeof(struct wdog) <= 0x38800bc0u, "the watchdog block must end before the flight recorder at 0xbc0");
/* While loop A is late (2 s and more), where it is: its saved PC and LR (FreeRTOS: the task's TCB starts with its saved
 * stack pointer; the context there is r4-r11 and EXC_RETURN, then s16-s31 if it used the FPU, then the exception frame),
 * a sample every 64 blocks into a ring of six. Backup SRAM 0x38800390..0x38800400 (after od.c's bank, before comp.c). */
struct asamp {
    uint32_t tcb, n;               /* task A's TCB (seen when it stamps); samples taken this run */
    uint32_t pc[1], lr[1];         /* the latest sample */
    uint32_t r_n, r_pc[1], r_lr[1];   /* at boot: the last run's */
    uint32_t r4, chain[5];         /* the latest sample's r4, its wake time, the kernel's next wake-up, the tick, the two delayed lists */
    uint32_t cont, evc;            /* the latest sample: the list its state item is on (ready / delayed / suspended), the list its event item is on */
    uint8_t prio, rbit, rcnt, rheal;   /* its priority; that ready list's priority bit and item count (at the sample); at boot: the last run's repairs */
    uint32_t rsum;                 /* the ready lists at the sample: with tasks (bits 0-6), with a task of another priority (8-14), uxTopReadyPriority (16-22) */
};
#define AS ((volatile struct asamp *)0x38800390u)
#define CUR_TCB (*(volatile uint32_t *)0x2401f2a8u)    /* FreeRTOS pxCurrentTCB (the PendSV handler's literal) */
_Static_assert(0x38800390u + sizeof(struct asamp) <= 0x388003f0u, "the loop-A samples must end before the frozen CPU shares at 0x3f0");
/* The RTOS clock: is the tick running, and is the scheduler suspended (then no task switches and no timeout expires)?
 * Sampled every audio block: ticks a second, the longest run of blocks with the scheduler suspended this run, and then the
 * running task's TCB and (when the audio code runs in an interrupt) the interrupted task's PC. Live 0x38800930 (after MCL's
 * 0x920..0x930), the last run's figures at 0x3880096c (after the SD block). */
struct sched {
    uint32_t tick0;                /* the tick count at the last rate sample */
    uint16_t rate, streak, max, ipsr;
    uint16_t heal;                 /* ready lists found with their priority bit missing (and put right), since boot */
    uint8_t heal_p, _p;            /* the priority of the last one */
};
struct sched_r {
    uint16_t rate, max, streak, ipsr;
    uint32_t pc, tcb;
};
#define XTICK (*(volatile uint32_t *)0x2401f1a4u)        /* FreeRTOS xTickCount */
#define XSUSP (*(volatile uint32_t *)0x2401f180u)        /* uxSchedulerSuspended */
#define XNEXT (*(volatile uint32_t *)0x2401f188u)        /* xNextTaskUnblockTime (prvResetNextTaskUnblockTime, 0x08088a18) */
#define XDLY (*(volatile uint32_t *)0x2401f1f0u)         /* pxDelayedTaskList */
#define XOVF (*(volatile uint32_t *)0x2401f1ecu)         /* pxOverflowDelayedTaskList */
#define XTOP (*(volatile uint32_t *)0x2401f1a0u)         /* uxTopReadyPriority: one bit per priority with a ready task (vTaskSwitchContext's CLZ) */
#define XREADY ((volatile uint32_t *)0x2401f21cu)        /* pxReadyTasksLists[7], 20 bytes (5 words) each, uxNumberOfItems first */
#define SC ((volatile struct sched *)0x38800930u)
#define SCR ((volatile struct sched_r *)0x3880096cu)
#define SC_PC (*(volatile uint32_t *)0x38800384u)        /* (live: the interrupted PC and the TCB at the longest suspension) */
#define SC_TCB (*(volatile uint32_t *)0x38800388u)
_Static_assert(sizeof(struct sched) <= 16, "the scheduler block is 0x930..0x940");
/* Who has the CPU: the SysTick interrupt (crash_systick_thunk, vector 15) looks at the running task each tick and counts
 * it; the three busiest tasks of each 250 ticks (0.5 s) are kept with their shares, the rest as "other". Backup SRAM
 * 0x38800fdc..0x38801000 (the top of the page state's 256 bytes; looper_page.c keeps below 0xdc). The names are read from
 * the TCBs when shown (after the restart the tasks are made again in the same places). */
struct prof {
    uint32_t t[3];                 /* this window's tasks */
    uint8_t c[3], co, n, _p[3];    /* their ticks, the others', the window's ticks */
    uint32_t rt[3];                /* the last full window (at boot: the last run's): tasks and shares (%) */
    uint8_t rp[3], ro;
};
#define PROF ((volatile struct prof *)0x38800fdcu)
struct prof_r {                    /* at boot: the last run's last full window, frozen (the live one goes on) */
    uint32_t t[3];
    uint8_t p[3], o;
};
#define PROFR ((volatile struct prof_r *)0x388003f0u)
_Static_assert(0x388003f0u + sizeof(struct prof_r) <= 0x38800400u, "the frozen CPU shares must end before comp.c's state at 0x400");
_Static_assert(0x38800fdcu + sizeof(struct prof) <= 0x38801000u, "the CPU shares must fit 0xfdc..0x1000");
/* The SD card driver's read (stock sd_diskio 0x08045974): starts the DMA read and waits for the completion message (stock: 5000 ticks,
 * which at this kernel's 501 ticks a second is ~10 s). A read that never completes sends nothing, so two of them in the loader's
 * first folder scan are 20 s and the watchdog restarts the unit (build 76's record: the loader in that wait, the SD queue). Here
 * (patches/crash.py): the wait is 1000 ticks (~2 s), and a failed read is tried again (three tries), after the driver's queue is
 * drained (a late completion must not count for the retry) and the HAL is idle again. Writes are the stock ones. Counted, with the
 * HAL's state / error and the SDMMC's status at the last failure. Backup SRAM 0x38800940 (frozen copy at boot). */
struct sdst {
    uint32_t magic;
    uint16_t fails, recov, gaveup, reads;   /* (reads: the loader's reads that worked, this run) */
    uint32_t err, sta;
    uint16_t r_fails, r_recov, r_gaveup, r_reads;
    uint32_t r_err, r_sta;
};
#define SD ((volatile struct sdst *)0x38800940u)
#define SD_MAGIC 0x32534453u
_Static_assert(0x38800940u + sizeof(struct sdst) <= 0x3880096cu, "the SD block must end before the scheduler figures at 0x96c");
#define LIVE ((volatile struct live *)0x38800b20u)
#define REC ((volatile struct rec *)0x38800b38u)
#define LIVE_MAGIC 0x4556494cu
#define REC_MAGIC 0x31435243u
_Static_assert(sizeof(struct live) <= 24, "the live block must end before the record at 0xb38");
_Static_assert(0x38800b38u + sizeof(struct rec) <= 0x38800b98u, "the record must end before the watchdog block at 0xb98");

static int in_ram(uint32_t a)
{
    return (a >= 0x20000000u && a < 0x2001ffe0u) || (a >= 0x24000000u && a < 0x2407ffe0u) || (a >= 0x30000000u && a < 0x30047fe0u) ||
           (a >= 0x38000000u && a < 0x3800ffe0u);
}

static void fill_common(volatile struct rec *r)
{
    volatile struct live *v = LIVE;
    int ok = v->magic == LIVE_MAGIC;
    for (int i = 0; i < 3; i++)
        r->bc[i] = ok ? v->bc[i] : 0;
    r->blocks = ok ? v->blocks : 0;
    r->kbmin = ok ? v->kbmin : 0;
    r->kbnow = ok ? v->kbnow : 0;
    r->load = ok ? v->load : 0;
    r->peak = ok ? v->peak : 0;
}

/* Tests stub this one out (the emulator cannot reset). */
void __attribute__((noipa)) crash_reset(void)          /* (noipa: the callers must not assume it never returns, for the tests) */
{
    __asm volatile("dsb");
    SCB_AIRCR = 0x05fa0004u;                                      /* SYSRESETREQ */
    __asm volatile("dsb");
    for (;;) {
    }
}

static void begin(volatile struct rec *r, int kind)
{
    if (r->magic != REC_MAGIC) {
        r->magic = REC_MAGIC;
        r->boots = r->faults = 0;
    }
    r->faults++;
    r->kind = (uint8_t)kind;
    r->state = 1;
    r->pc = r->lr = r->xpsr = r->sp = r->exc = r->r0 = r->r12 = r->oom = 0;
    for (int i = 0; i < 4; i++)
        r->stk[i] = 0;
    r->cfsr = SCB_CFSR;
    r->hfsr = SCB_HFSR;
    r->mmfar = SCB_MMFAR;
    r->bfar = SCB_BFAR;
    fill_common(r);
}

/* From crash_fault: sp = the exception frame (r0 r1 r2 r3 r12 lr pc xpsr), exc = EXC_RETURN, ipsr = the exception number. */
void crash_record(const uint32_t *sp, uint32_t exc, uint32_t ipsr)
{
    volatile struct rec *r = REC;
    int n = (int)(ipsr & 0x1ff);
    begin(r, n == 3 ? 1 : n == 4 ? 2 : n == 5 ? 3 : n == 6 ? 4 : 1);
    r->sp = (uint32_t)sp;
    r->exc = exc;
    if (in_ram((uint32_t)sp)) {
        r->r0 = sp[0];
        r->r12 = sp[4];
        r->lr = sp[5];
        r->pc = sp[6];
        r->xpsr = sp[7];
        int ext = !(exc & 0x10u);                                 /* a frame with the FPU's registers is 26 words, else 8 */
        const uint32_t *above = sp + (ext ? 26 : 8);
        for (int i = 0; i < 4; i++)
            r->stk[i] = in_ram((uint32_t)(above + i)) ? above[i] : 0;
    }
    crash_reset();
}

/* From crash_oom_thunk, in place of the stock allocator's message before it halts: size = what was asked for (rounded up to 16), caller = who asked. */
void crash_oom(const char *msg, uint32_t size, uint32_t caller)
{
    (void)msg;
    volatile struct rec *r = REC;
    begin(r, 5);
    r->oom = size;
    r->lr = caller;
    crash_reset();
}

void crash_boot(void)
{
    volatile struct rec *r = REC;
    volatile struct live *v = LIVE;
    uint32_t rsr = RCC_RSR;
    RCC_RSR = rsr | (1u << 16);                                   /* RMVF: the next restart's flags start from 0 */
    if (r->magic != REC_MAGIC) {
        r->magic = REC_MAGIC;
        r->boots = r->faults = 0;
        r->state = 0;
        r->kind = 0;
    }
    if (r->state != 1) {                                          /* no handler wrote it: the last run ended without a fault (the watchdog, power) */
        r->kind = 0;
        r->pc = r->lr = r->xpsr = r->sp = r->exc = r->cfsr = r->hfsr = r->mmfar = r->bfar = r->r0 = r->r12 = r->oom = 0;
        for (int i = 0; i < 4; i++)
            r->stk[i] = 0;
        fill_common(r);
    }
    {
        volatile struct wdog *w = WD;
        int ok = v->magic == LIVE_MAGIC;
        w->r_lag_a = ok ? w->lag_a : 0;
        w->r_lag_b = ok ? w->lag_b : 0;
        w->r_site = ok ? w->site_a : 0;
        volatile struct asamp *as = AS;
        uint32_t n = ok ? as->n : 0;
        as->r_n = n;
        as->r_pc[0] = n ? as->pc[0] : 0;
        as->r_lr[0] = n ? as->lr[0] : 0;
        as->n = 0;
        as->tcb = 0;
        volatile struct sched *sk = SC;
        volatile struct sched_r *kr = SCR;
        kr->rate = ok ? sk->rate : 0;
        kr->max = ok ? sk->max : 0;
        kr->streak = ok ? sk->streak : 0;
        kr->ipsr = ok ? sk->ipsr : 0;
        as->rheal = (uint8_t)(ok ? (sk->heal > 255u ? 255u : sk->heal) : 0);
        volatile struct prof *f = PROF;
        volatile struct prof_r *fr = PROFR;
        for (int i = 0; i < 3; i++) {
            fr->t[i] = f->rt[i];
            fr->p[i] = f->rp[i];
            f->rt[i] = 0;
            f->rp[i] = 0;
            f->t[i] = 0;
            f->c[i] = 0;
        }
        fr->o = f->ro;
        f->ro = 0;
        f->co = f->n = 0;
        sk->tick0 = 0;
        sk->rate = sk->streak = sk->max = sk->ipsr = 0;
        sk->heal = 0;
        sk->heal_p = 0;
        w->cyc_au = w->cyc_gui = w->cyc_a = w->site_a = w->per = 0;
        w->lag_a = w->lag_b = 0;
    }
    {
        volatile struct sdst *q = SD;
        int ok = q->magic == SD_MAGIC;
        q->r_fails = ok ? q->fails : 0;
        q->r_recov = ok ? q->recov : 0;
        q->r_gaveup = ok ? q->gaveup : 0;
        q->r_reads = ok ? q->reads : 0;
        q->r_err = ok ? q->err : 0;
        q->r_sta = ok ? q->sta : 0;
        q->magic = SD_MAGIC;
        q->fails = q->recov = q->gaveup = q->reads = 0;
        q->err = q->sta = 0;
    }
    r->state = 2;                                                 /* (not looked at yet) */
    r->rsr = rsr;
    r->boots++;
    v->magic = LIVE_MAGIC;
    for (int i = 0; i < 4; i++)
        v->bc[i] = 0;
    v->blocks = 0;
    v->kbmin = 0xffff;
    v->kbnow = 0;
    v->load = v->peak = 0;
}

void crash_tick(void)
{
    volatile struct live *v = LIVE;
    if (v->magic != LIVE_MAGIC)
        return;
    uint32_t b = v->blocks + 1;
    v->blocks = b;
    if ((b & 15) == 0) {                                          /* every 16th block (~85 ms) */
        unsigned kb = lira_free_kb();
        if (kb > 0xffff)
            kb = 0xffff;
        v->kbnow = (uint16_t)kb;
        if (kb < v->kbmin)
            v->kbmin = (uint16_t)kb;
        unsigned ld = looper_load_avg();
        v->load = (uint16_t)ld;
        if (ld > v->peak && b > 400)                              /* (not the start-up) */
            v->peak = (uint16_t)ld;
    }
}

uint32_t __attribute__((noipa)) crash_ipsr(void)                  /* (functions of their own: the tests stub them) */
{
    uint32_t v;
    __asm volatile("mrs %0, ipsr" : "=r"(v));
    return v;
}

uint32_t __attribute__((noipa)) crash_irq_off(void)
{
    uint32_t v;
    __asm volatile("mrs %0, primask\n cpsid i" : "=r"(v) :: "memory");
    return v;
}

void __attribute__((noipa)) crash_irq_on(uint32_t v)
{
    __asm volatile("msr primask, %0" :: "r"(v) : "memory");
}


/* In place of the audio block's watchdog check (bl @0x0804cb36): obj = {stamp A, stamp B, blocks}. */
void crash_wd(volatile uint32_t *obj)
{
    volatile struct wdog *w = WD;
    uint32_t now = CYC, d = now - w->cyc_au;
    if (w->cyc_au && d > 100000u && d < 100000000u)
        w->per = w->per ? w->per + (d - w->per) / 16u : d;
    w->cyc_au = now;
    uint32_t c = obj[2];
    w->lag_a = (uint16_t)((c - obj[0]) > 0xffffu ? 0xffffu : c - obj[0]);
    w->lag_b = (uint16_t)((c - obj[1]) > 0xffffu ? 0xffffu : c - obj[1]);
    {
        volatile struct sched *k = SC;
        uint32_t ip = crash_ipsr();
        k->ipsr = (uint16_t)(ip & 0x1ffu);
        if ((c % 188u) == 0) {                                    /* about once a second: ticks since the last time */
            uint32_t t = XTICK, dt = t - k->tick0;
            k->rate = (uint16_t)(dt > 0xffffu ? 0xffffu : dt);
            k->tick0 = t;
        }
        if (XSUSP) {
            if (k->streak < 0xffffu)
                k->streak++;
            if (k->streak > k->max)
                k->max = k->streak;
        } else {
            k->streak = 0;
        }
    }
    volatile struct asamp *as = AS;
    if (w->lag_a >= 375u && (w->lag_a & 63u) == 0 && in_ram(as->tcb) && LIVE->blocks > 5600u) {   /* loop A 2 s late: where is it? (not in the
                                                                   first 30 s: it is late while the unit starts, and the last run's samples must stay) */
        const uint32_t *sp = *(const uint32_t *const *)as->tcb;
        if (in_ram((uint32_t)sp) && in_ram((uint32_t)(sp + 40))) {
            uint32_t exc = sp[8];
            const uint32_t *hw = sp + 9 + ((exc & 0x10u) ? 0 : 16);
            uint32_t k = 0;
            as->pc[k] = hw[6];
            as->lr[k] = hw[5];
            as->n++;
            as->r4 = sp[0];
            as->cont = ((const uint32_t *)as->tcb)[5];            /* (TCB: +0x14 xStateListItem.pvContainer, +0x28 xEventListItem.pvContainer, +0x2c uxPriority) */
            as->evc = ((const uint32_t *)as->tcb)[10];
            as->prio = (uint8_t)((const uint32_t *)as->tcb)[11];
            if (as->prio < 7) {
                as->rbit = (uint8_t)((XTOP >> as->prio) & 1u);
                uint32_t cn = XREADY[5 * as->prio];
                as->rcnt = (uint8_t)(cn > 255u ? 255u : cn);
            }
            int m = 0;                                            /* its callers: odd code addresses above its frame */
            const uint32_t *q = hw + ((exc & 0x10u) ? 8 : 26);
            for (int i = 0; i < 96 && m < 5 && in_ram((uint32_t)(q + i)); i++)
                if ((q[i] & 1u) && q[i] >= 0x08040001u && q[i] < 0x08140000u)
                    as->chain[m++] = q[i];
            for (; m < 5; m++)
                as->chain[m] = 0;
        }
    }
    ((void (*)(volatile uint32_t *))0x08046995u)(obj);
}

void __attribute__((noinline)) crash_stamp_a(volatile uint32_t *obj)    /* in place of the A stamps (0x080469c4) */
{
    WD->cyc_a = CYC;
    WD->site_a = (uint32_t)__builtin_return_address(0) - 5u;      /* (the bl's address) */
    AS->tcb = CUR_TCB;                                            /* (this runs in loop A's task) */
    obj[0] = obj[2];
}

void crash_stamp_b(volatile uint32_t *obj)                        /* in place of the GUI's stamp (0x080469ca) */
{
    WD->cyc_gui = CYC;
    obj[1] = obj[2];
}

void crash_systick(void)                                          /* from the SysTick handler, before the stock one */
{
    volatile struct prof *f = PROF;
    uint32_t t = CUR_TCB;
    int k = 0;
    while (k < 3 && f->t[k] && f->t[k] != t)
        k++;
    if (k < 3) {
        f->t[k] = t;
        f->c[k]++;
    } else {
        f->co++;
    }
    if (++f->n >= 250u) {
        for (int i = 0; i < 3; i++) {
            f->rt[i] = f->t[i];
            f->rp[i] = (uint8_t)(f->c[i] * 2u / 5u);
            f->t[i] = 0;
            f->c[i] = 0;
        }
        f->ro = (uint8_t)(f->co * 2u / 5u);
        f->co = 0;
        f->n = 0;
    }
}


static int sd_settle(void)
{
    typedef void (*get_fn)(uint32_t *ev, uint32_t queue, uint32_t ms);
    typedef uint32_t (*tick_fn)(void);
    uint32_t ev[3];
    for (int i = 0; i < 8; i++) {                                 /* drain the driver's queue (osMessageGet, no wait) */
        ((get_fn)0x08087be9u)(ev, *(volatile uint32_t *)0x24009bf8u, 0);
        if (ev[0] != 0x10u)
            break;
    }
    uint32_t t0 = ((tick_fn)0x08087a5du)();                       /* osKernelSysTick */
    while (*(volatile uint8_t *)(0x24009bfcu + 0x30) != 1)        /* the HAL's state back to READY (1), up to 400 ticks (~0.8 s) */
        if (((tick_fn)0x08087a5du)() - t0 > 400u)
            return 0;
    return 1;
}

int crash_sd_read(uint8_t lun, uint8_t *buf, uint32_t sector, uint32_t count)     /* in the driver table, for 0x08045975 */
{
    volatile struct sdst *q = SD;
    for (int t = 0; t < 3; t++) {
        if (t && !sd_settle())
            break;
        if (((int (*)(uint8_t, uint8_t *, uint32_t, uint32_t))0x08045975u)(lun, buf, sector, count) == 0) {
            if (t && q->recov < 0xffffu)
                q->recov++;
            if (CUR_TCB == AS->tcb) {                             /* the loader: a read that worked is progress. Its check-in with the stock
                                                                     watchdog (stamp A) comes only between jobs, and the first folder scan is one
                                                                     job of many reads: under load (and with a pad recording to the card) it
                                                                     takes over 20 s and the watchdog restarts the unit (build 78's record: the
                                                                     loader alternately waiting on the SD queue and ready, no read failing). */
                volatile uint32_t *wd = (volatile uint32_t *)0x2400a584u;   /* {stamp A, stamp B, blocks} */
                wd[0] = wd[2];
                if (q->reads < 0xffffu)
                    q->reads++;
            }
            return 0;
        }
        if (q->fails < 0xffffu)
            q->fails++;
        volatile uint8_t *h = (volatile uint8_t *)0x24009bfcu;    /* the HAL's SD handle: +0x30 state, +0x34 error code */
        q->err = (uint32_t)h[0x30] << 24 | (*(volatile uint32_t *)(h + 0x34) & 0xffffffu);
        q->sta = *(volatile uint32_t *)0x52007034u;               /* SDMMC1 STA */
    }
    if (q->gaveup < 0xffffu)
        q->gaveup++;
    return 1;
}

int crash_pending(void)
{
    return REC->magic == REC_MAGIC && REC->state == 2 && (REC->kind || (REC->rsr & ((1u << 26) | (1u << 28))));
}

void crash_clear(void)
{
    volatile struct rec *r = REC;
    if (r->magic == REC_MAGIC) {
        r->kind = 0;
        r->state = 3;
        r->rsr = 0;
    }
}

/* ---- text ---- */
static char *put_s(char *p, char *e, const char *s)
{
    while (*s && p < e)
        *p++ = *s++;
    return p;
}

static char *put_x(char *p, char *e, uint32_t v)
{
    for (int i = 7; i >= 0 && p < e; i--) {
        unsigned d = (v >> (4 * i)) & 15;
        *p++ = (char)(d < 10 ? '0' + d : 'A' + d - 10);
    }
    return p;
}

static char *put_u(char *p, char *e, uint32_t v)
{
    char t[10];
    int n = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 10);
    while (n && p < e)
        *p++ = t[--n];
    return p;
}

static const char *bc_name(int id)
{
    switch (id) {
    case 0: return "-";
    case BCA_STAGE: return "STAGE";
    case BCA_DRONE: return "DRONE";
    case BCA_SAMPLR: return "SAMPLR";
    case BCA_TRACKS: return "LOOPER";
    case BCA_RECFILL: return "RECFILL";
    case BCA_IN: return "INPUT";
    case BCG_MSG: return "MSG";
    case BCG_PAINT: return "PAINT";
    case BCG_TOUCH: return "TOUCH";
    case BCG_SCREEN: return "SCREEN";
    case BCF_SAVE: return "SAVE";
    case BCF_LOAD: return "LOAD";
    default: return "?";
    }
}

static const char *reset_name(uint32_t f)
{
    if (f & (1u << 26))
        return "WATCHDOG";
    if (f & (1u << 28))
        return "WINDOW WDG";
    if (f & (1u << 24))
        return "SOFTWARE";
    if (f & (1u << 21))
        return "BROWNOUT";
    if (f & (1u << 23))
        return "POWER ON";
    if (f & (1u << 30))
        return "LOW POWER";
    if (f & (1u << 22))
        return "RESET PIN";
    return f ? "OTHER" : "-";
}

int crash_line(int i, char *buf, int n)
{
    volatile struct rec *r = REC;
    char *p = buf, *e = buf + n - 1;
    if (r->magic != REC_MAGIC) {
        if (i)
            return 0;
        p = put_s(p, e, "NO RECORD YET");
        *p = 0;
        return 1;
    }
    static const char *const kinds[6] = {"NO FAULT", "HARD FAULT", "MEMORY FAULT", "BUS FAULT", "USAGE FAULT", "OUT OF MEMORY"};
    if (i >= 1 && i <= 4 && r->kind == 0)
        return 0;                                                 /* (the fault's registers: only when there was one) */
    switch (i) {
    case 0:
        p = put_s(p, e, "LAST RESTART: ");
        p = put_s(p, e, reset_name(r->rsr));
        p = put_s(p, e, " / ");
        p = put_s(p, e, kinds[r->kind < 6 ? r->kind : 0]);
        break;
    case 1:
        p = put_s(p, e, "PC ");
        p = put_x(p, e, r->pc);
        p = put_s(p, e, "  LR ");
        p = put_x(p, e, r->lr);
        break;
    case 2:
        p = put_s(p, e, "CFSR ");
        p = put_x(p, e, r->cfsr);
        p = put_s(p, e, " HFSR ");
        p = put_x(p, e, r->hfsr);
        break;
    case 3:
        p = put_s(p, e, "ADDR ");
        p = put_x(p, e, (r->cfsr & 0x80u) ? r->mmfar : r->bfar);
        p = put_s(p, e, " SP ");
        p = put_x(p, e, r->sp);
        break;
    case 4:
        p = put_s(p, e, "STK ");
        p = put_x(p, e, r->stk[0]);
        p = put_s(p, e, " ");
        p = put_x(p, e, r->stk[1]);
        p = put_s(p, e, " ");
        p = put_x(p, e, r->stk[2]);
        break;
    case 5:
        p = put_s(p, e, "IN: AUDIO ");
        p = put_s(p, e, bc_name(r->bc[0]));
        p = put_s(p, e, " GUI ");
        p = put_s(p, e, bc_name(r->bc[1]));
        p = put_s(p, e, " FILE ");
        p = put_s(p, e, bc_name(r->bc[2]));
        break;
    case 6:
        p = put_s(p, e, "RAN ");
        p = put_u(p, e, r->blocks / 187u);                       /* (256-frame blocks at 48 kHz: 187.5 a second) */
        p = put_s(p, e, " S  RAM MIN ");
        p = put_u(p, e, r->kbmin == 0xffff ? 0 : r->kbmin);
        p = put_s(p, e, " KB NOW ");
        p = put_u(p, e, r->kbnow);
        break;
    case 7:
        p = put_s(p, e, "LOAD ");
        p = put_u(p, e, r->load / 10u);
        p = put_s(p, e, "% PEAK ");
        p = put_u(p, e, r->peak / 10u);
        p = put_s(p, e, "%");
        if (r->kind == 5) {
            p = put_s(p, e, "  ASKED ");
            p = put_u(p, e, r->oom);
        }
        break;
    case 8:
        p = put_s(p, e, "BOOTS ");
        p = put_u(p, e, r->boots);
        p = put_s(p, e, "  FAULTS ");
        p = put_u(p, e, r->faults);
        p = put_s(p, e, "  RSR ");
        p = put_x(p, e, r->rsr);
        break;
    case 10: {
        volatile struct wdog *w = WD;
        p = put_s(p, e, "LAG A ");
        p = put_u(p, e, w->r_lag_a);
        p = put_s(p, e, " B ");
        p = put_u(p, e, w->r_lag_b);
        p = put_s(p, e, "  A AT ");
        p = put_x(p, e, w->r_site);
        break;
    }
    case 11: {
        volatile struct asamp *a = AS;
        if (!a->r_n) {
            p = put_s(p, e, "LOOP A WAS NEVER LATE");
            break;
        }
        p = put_s(p, e, "A LATE AT PC ");
        p = put_x(p, e, a->r_pc[0]);
        p = put_s(p, e, " LR ");
        p = put_x(p, e, a->r_lr[0]);
        p = put_s(p, e, " N ");
        p = put_u(p, e, a->r_n);
        break;
    }
    case 14:
    case 15: {
        volatile struct asamp *a = AS;
        if (!a->r_n)
            return 0;
        p = put_s(p, e, i == 14 ? "OBJ " : "");
        if (i == 14)
            p = put_x(p, e, a->r4);
        for (int k = (i - 14) * 3; k < 5 && k < (i - 14) * 3 + 3; k++) {
            p = put_s(p, e, " ");
            p = put_x(p, e, a->chain[k]);
        }
        break;
    }
    case 18: {
        volatile struct sched_r *kr = SCR;
        p = put_s(p, e, "TICKS/S ");
        p = put_u(p, e, kr->rate);
        p = put_s(p, e, " SUSPENDED MAX ");
        p = put_u(p, e, kr->max);
        p = put_s(p, e, " NOW ");
        p = put_u(p, e, kr->streak);
        p = put_s(p, e, " IRQ ");
        p = put_u(p, e, kr->ipsr);
        break;
    }
    case 9: {
        volatile struct sdst *q = SD;
        p = put_s(p, e, "SD READ FAILS ");
        p = put_u(p, e, q->r_fails);
        p = put_s(p, e, " OK AGAIN ");
        p = put_u(p, e, q->r_recov);
        p = put_s(p, e, " GAVE UP ");
        p = put_u(p, e, q->r_gaveup);
        p = put_s(p, e, " READS ");
        p = put_u(p, e, q->r_reads);
        if (q->r_fails) {
            p = put_s(p, e, " ");
            p = put_x(p, e, q->r_err);
            p = put_s(p, e, "/");
            p = put_x(p, e, q->r_sta);
        }
        break;
    }
    case 20:
    case 21: {
        volatile struct prof_r *f = PROFR;
        p = put_s(p, e, i == 20 ? "CPU " : "    ");
        for (int k = (i - 20) * 2; k < 3 && k < (i - 20) * 2 + 2; k++) {
            uint32_t t = f->t[k];
            if (!t)
                continue;
            const volatile char *nm = (const volatile char *)(t + 0x34);       /* pcTaskName */
            int ok = in_ram(t);
            for (int c = 0; c < 8 && ok && nm[c] >= 0x20 && nm[c] < 0x7f; c++)
                if (p < e)
                    *p++ = nm[c];
            if (!ok || !(nm[0] >= 0x20 && nm[0] < 0x7f))
                p = put_x(p, e, t);
            p = put_s(p, e, " ");
            p = put_u(p, e, f->p[k]);
            p = put_s(p, e, "%  ");
        }
        if (i == 21) {
            p = put_s(p, e, "REST ");
            p = put_u(p, e, f->o);
            p = put_s(p, e, "%");
        }
        break;
    }
    case 22: {
        volatile struct asamp *a = AS;
        if (!a->r_n)
            return 0;
        p = put_s(p, e, "LOADER P");
        p = put_u(p, e, a->prio);
        p = put_s(p, e, " ON ");
        p = put_x(p, e, a->cont);
        p = put_s(p, e, " EV ");
        p = put_x(p, e, a->evc);
        p = put_s(p, e, " BIT ");
        p = put_u(p, e, a->rbit);
        p = put_s(p, e, " N ");
        p = put_u(p, e, a->rcnt);
        break;
    }
    case 23: {
        volatile struct sched *k = SC;
        p = put_s(p, e, "REPAIRS LAST ");
        p = put_u(p, e, AS->rheal);
        p = put_s(p, e, " NOW ");
        p = put_u(p, e, k->heal);
        if (k->heal) {
            p = put_s(p, e, " (PRIO ");
            p = put_u(p, e, k->heal_p);
            p = put_s(p, e, ")");
        }
        p = put_s(p, e, " IRQ<5:");
        int m = 0;
        volatile const uint32_t *iser = (volatile const uint32_t *)0xe000e100u;
        volatile const uint8_t *ipr = (volatile const uint8_t *)0xe000e400u;
        for (int n = 0; n < 160 && m < 4; n++)                     /* enabled interrupts more urgent than FreeRTOS allows (BASEPRI 0x50) */
            if ((iser[n >> 5] >> (n & 31)) & 1u && ipr[n] < 0x50u) {
                p = put_s(p, e, " ");
                p = put_u(p, e, (uint32_t)n);
                p = put_s(p, e, "/");
                p = put_u(p, e, ipr[n] >> 4);
                m++;
            }
        if (!m)
            p = put_s(p, e, " NONE");
        break;
    }
    default:
        return 0;
    }
    *p = 0;
    return 1;
}
