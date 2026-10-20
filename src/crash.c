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
        int ok = v->magic == LIVE_MAGIC && w->per > 100000u && w->per < 100000000u;
        float k = ok ? (256000.f / 48000.f) / (float)w->per : 0.f;   /* ms a cycle */
        float g = (float)(int32_t)(w->cyc_gui - w->cyc_au) * k, a = (float)(int32_t)(w->cyc_a - w->cyc_au) * k;
        w->r_gui = (int16_t)(g > 32000.f ? 32000.f : g < -32000.f ? -32000.f : g);
        w->r_a = (int16_t)(a > 32000.f ? 32000.f : a < -32000.f ? -32000.f : a);
        w->r_lag_a = ok ? w->lag_a : 0;
        w->r_lag_b = ok ? w->lag_b : 0;
        w->r_site = ok ? w->site_a : 0;
        w->cyc_au = w->cyc_gui = w->cyc_a = w->site_a = w->per = 0;
        w->lag_a = w->lag_b = 0;
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
    ((void (*)(volatile uint32_t *))0x08046995u)(obj);
}

void __attribute__((noinline)) crash_stamp_a(volatile uint32_t *obj)    /* in place of the A stamps (0x080469c4) */
{
    WD->cyc_a = CYC;
    WD->site_a = (uint32_t)__builtin_return_address(0) - 5u;      /* (the bl's address) */
    obj[0] = obj[2];
}

void crash_stamp_b(volatile uint32_t *obj)                        /* in place of the GUI's stamp (0x080469ca) */
{
    WD->cyc_gui = CYC;
    obj[1] = obj[2];
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

static char *put_i(char *p, char *e, int v)
{
    if (v < 0 && p < e) {
        *p++ = '-';
        v = -v;
    } else if (p < e) {
        *p++ = '+';
    }
    return put_u(p, e, (uint32_t)v);
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
    case 9: {
        volatile struct wdog *w = WD;
        p = put_s(p, e, "AFTER AUDIO: GUI ");
        p = put_i(p, e, w->r_gui);
        p = put_s(p, e, "MS  LOOP A ");
        p = put_i(p, e, w->r_a);
        p = put_s(p, e, "MS");
        break;
    }
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
    default:
        return 0;
    }
    *p = 0;
    return 1;
}
