/* Flash bank 2, bank 1 side: the pairing check (b2.h). The first call of b2_gate() reads the payload; interrupts are off for it (a few ms, at boot). */
#include "b2.h"
#include "crash.h"

#define FLASH_SR2   (*(volatile uint32_t *)0x52002110u)
#define FLASH_CCR2  (*(volatile uint32_t *)0x52002114u)   /* bank 2's flag-clear register */
#define FLASH_ECC   ((1u << 25) | (1u << 26))             /* SNECCERR | DBECCERR */
#define DCIMVAC     (*(volatile uint32_t *)0xe000ef5cu)   /* data cache: forget the line holding this address */
#define B2_SCANS    16

/* Which pages the image has (src/looper_page.c reads them: they live here, in bank 1, so patch.py can set them whatever happens to bank 2).
 * bb_host is set by patches/looper.py: without the hooks of `looper` there is no page at all; patches/hide_*.py set a byte of bb_hide to 1 to take a page out of the round. */
const volatile uint8_t bb_host = 0;
const volatile uint8_t bb_hide[4] = {0, 0, 0, 0};

/* The guard stubs of the hooks that jump into bank 2 (patches/b2guard.py writes them: 64 bytes a site, 0xFF until then). */
__attribute__((used, aligned(64))) const uint8_t b2_stub_area[56 * 64] = {[0 ... 56 * 64 - 1] = 0xff};

__attribute__((used)) const volatile uint32_t b2_expect_sum = 0xffffffffu;   /* set by patch.py; 0xffffffff = the image has no bank 2 */

static int payload_ok(void)
{
    const volatile uint32_t *h = (const volatile uint32_t *)B2_BASE;
    if (b2_expect_sum == 0xffffffffu || h[0] != B2_MAGIC || h[1] != 1u)
        return 0;
    uint32_t len = h[2];
    if (len < B2_HDR + 32u || len > B2_LIMIT || (len & 31u) || h[3] != b2_expect_sum)
        return 0;
    uint32_t io = h[4], il = h[5], id = h[6];
    if ((io & 31u) || io < B2_HDR || il > 0x10000u || io + il > len || (id & 3u) || id < 0x200u || id + il > 0x10000u)
        return 0;
    uint32_t sum = 0;
    for (uint32_t i = B2_HDR; i < len; i += 4u)
        sum += *(const volatile uint32_t *)(B2_BASE + i);
    if (sum != b2_expect_sum)
        return 0;
    /* This chip corrects one flipped bit per 256-bit word silently and bus-faults on two (JOEBOX: a marginal write passes every sum, then kills a fetch). So read every word
     * B2_SCANS times with the data cache forgotten first, and refuse the lot if an ECC flag went up. A dead scan leaves B2_MARK_SCANNING behind (b2_decide). */
    FLASH_CCR2 = FLASH_ECC;
    for (int pass = 0; pass < B2_SCANS; pass++)
        for (uint32_t a = B2_BASE; a < B2_BASE + len; a += 32u) {
            DCIMVAC = a;
            (void)*(const volatile uint32_t *)a;
        }
    if (FLASH_SR2 & FLASH_ECC)
        return 0;
    /* the machines' ITCM code, from the payload (the core's own is copied by cpu.c) */
    volatile uint32_t *d = (volatile uint32_t *)id;
    const volatile uint32_t *s = (const volatile uint32_t *)(B2_BASE + io);
    for (uint32_t n = il / 4u; n; n--)
        *d++ = *s++;
    __asm__ volatile("dsb\n isb" ::: "memory");
    return 1;
}

static int b2_decide(void)
{
    if (b2_expect_sum == 0xffffffffu) {               /* the image has no bank 2 (no machines): nothing to read, no hardware to touch */
        B2_VERDICT = B2_UNPAIRED;
        return 0;
    }
    uint32_t primask = crash_irq_off();
    int ok = 0;
    uint32_t v = B2_VERDICT;
    if (v == B2_PAIRED || v == B2_UNPAIRED) {         /* another task got there while we waited for the mask */
        ok = v == B2_PAIRED;
    } else {
        bkp_enable();
        uint32_t m = *B2_MARK;
        if (m != B2_MARK_SCANNING && m != B2_MARK_CLAIMING) {      /* (backup SRAM is random after a cold power-up: only these two values mean a dead look) */
            bkp_put(B2_MARK, B2_MARK_SCANNING);
            ok = payload_ok();
            bkp_put(B2_MARK, 0);
        }
        B2_VERDICT = ok ? B2_PAIRED : B2_UNPAIRED;
    }
    crash_irq_on(primask);
    return ok;
}

int b2_gate(void)
{
    uint32_t v = B2_VERDICT;
    if (v == B2_PAIRED)
        return 1;
    if (v == B2_UNPAIRED)
        return 0;
    return b2_decide();
}

void b2_boot(void)
{
    B2_VERDICT = 0;
    B2_STATE = 0;
}

/* The loader has just rewritten bank 2: the marks of a dead scan are stale, and the caches may hold the old words. */
int b2_rescan(void)
{
    bkp_enable();
    bkp_put(B2_MARK, 0);
    B2_VERDICT = 0;
    return b2_decide();
}
