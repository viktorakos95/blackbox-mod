/*
 * The card loader: flash bank 2 written from BBEXTRA.BIN on the SD card. Original 1010music Blackbox, firmware 3.1.9.
 * Design and recipe: j3threejay/blackbox-mod's src/b2load.c (MIT, THIRD_PARTY.md), proven on their unit; changed here only where noted.
 *
 * WHERE IT RUNS: stock's FatFs is not re-entrant, and stock keeps it safe by doing all card access in one task (pcmStreamer, whose loop FUN_080743b0
 * serves the file commands). So this is called from that loop (b2load_thunk.S), never from the audio task. The window watchdog is fed from the
 * audio block only while both monitored tasks have stamped within 10 s; this task's stamp is on 0x2400a584: anything long in here stamps too.
 *
 * WHAT MAY BE WRITTEN: bank 2 through bank 2's own registers (KEYR2 / CR2 / CCR2): sectors 0-5 erased and programmed, one claim word programmed once at
 * the start of sector 6. NEVER bank 1's registers (bank 1 holds the bootloader) and never sector 7 (0x081E0000, the factory calibration).
 * It only writes when the pairing check said "not paired", so nothing executes up there; the header (the flash word that makes a payload valid) goes on LAST,
 * so a write cut short by the power leaves something the next boot sees as no payload and simply does again.
 *
 * WHOSE IS IT (owned): the loader erases, and an erase cannot be taken back. Before the first erase on a unit the bank must be BLANK, or look exactly like
 * what this mod's own earlier images left there when the stock installer wrote them past the end of bank 1 (legacy(), below: the cave's tail, then 0xFF, then
 * either the MCL program at 0x08120000 or nothing up to the ITCM image at 0x081D0000). Then a claim word is written at the start of sector 6, which is
 * never erased, and from then on the claim is the answer. Anything else up there is left exactly as it is (B2_FOREIGN).
 */
#include "b2.h"

#define FN(addr) ((addr) | 1u)
typedef int (*f_open_fn)(void *file, const char *path, unsigned mode);
typedef int (*f_read_fn)(void *file, void *to, unsigned bytes, unsigned *got);
typedef int (*f_close_fn)(void *file);
#define fw_f_open   ((f_open_fn)FN(0x08086534))
#define fw_f_read   ((f_read_fn)FN(0x08086866))
#define fw_f_close  ((f_close_fn)FN(0x08086d76))
#define FIL_BYTES   0x58         /* stock clears this much before every f_open (FUN_080971cc): FatFs built tiny */
#define FA_READ     1u

static const char B2_NAME[] = "BBEXTRA.BIN";     /* 8.3, root of the card */

#define CARD_FOUND  (*(const volatile uint8_t *)(0x240021a4u + 0x15u))  /* stock's own "the root opened" (FUN_08042c90) */
#define WDG_DISK    (*(volatile uint32_t *)0x2400a584u)                 /* this task's stamp ... */
#define WDG_NOW     (*(const volatile uint32_t *)0x2400a58cu)           /* ... and the counter it copies */

#define FLASH_KEYR2 (*(volatile uint32_t *)0x52002104u)
#define FLASH_CR2   (*(volatile uint32_t *)0x5200210cu)
#define FLASH_SR2   (*(volatile uint32_t *)0x52002110u)
#define FLASH_CCR2  (*(volatile uint32_t *)0x52002114u)
#define KEY1        0x45670123u
#define KEY2        0xcdef89abu
#define CR_LOCK     (1u << 0)
#define CR_PG       (1u << 1)
#define CR_SER      (1u << 2)
#define CR_PSIZE64  (3u << 4)
#define CR_START    (1u << 7)
#define SR_BUSY     ((1u << 0) | (1u << 1) | (1u << 2))                /* BSY | WBNE | QW */
#define SR_EOP      (1u << 16)
#define SR_ERRORS   ((1u << 17) | (1u << 18) | (1u << 19) | (1u << 21) | (1u << 22) | (1u << 23) | (1u << 24))
#define SR_ECC      ((1u << 25) | (1u << 26))
#define DCIMVAC     (*(volatile uint32_t *)0xe000ef5cu)
#define ICIALLU     (*(volatile uint32_t *)0xe000ef50u)

#define SPINS_ERASE 400000000u   /* loop counts, not times: an erase measured 0.87 s on their unit */
#define SPINS_WORD  20000000u

static void stamp(void)
{
    WDG_DISK = WDG_NOW;
}

struct src {
    uint32_t file[FIL_BYTES / 4];
};

static int src_open(struct src *s)
{
    for (unsigned i = 0; i < FIL_BYTES / 4; i++)
        s->file[i] = 0;
    return fw_f_open(s->file, B2_NAME, FA_READ) == 0;
}

static int src_word(struct src *s, uint32_t w[8])        /* the file is read a flash word (32 bytes) at a time */
{
    unsigned got = 0;
    return fw_f_read(s->file, w, 32u, &got) == 0 && got == 32u;
}

/* Is this first flash word the header of the payload THIS image was built for? */
static int header_ok(const uint32_t w[8])
{
    uint32_t len = w[2];
    return w[0] == B2_MAGIC && w[1] == 1u && len >= 2u * B2_HDR && len <= B2_LIMIT && !(len & 31u) && w[3] == b2_expect_sum && b2_expect_sum != 0xffffffffu;
}

static unsigned file_verdict(void)       /* read the whole file and say what it is. Nothing is erased on the strength of a header alone. */
{
    struct src s;
    uint32_t w[8];
    if (!src_open(&s))
        return B2_FILE_NONE;
    unsigned verdict = B2_FILE_WRONG;
    if (src_word(&s, w) && header_ok(w)) {
        uint32_t len = w[2], want = w[3], sum = 0, at = 32u;
        for (; at < len && src_word(&s, w); at += 32u) {
            for (int i = 0; i < 8; i++)
                sum += w[i];
            if (!(at & 0xffffu))
                stamp();
        }
        if (at >= len && sum == want)
            verdict = B2_FILE_OK;
    }
    fw_f_close(s.file);
    return verdict;
}

static inline void barrier(void)
{
    __asm__ volatile("isb\n dsb");
}

static int flash_wait(uint32_t spins)
{
    while (FLASH_SR2 & SR_BUSY)
        if (!spins--)
            return 0;
    return !(FLASH_SR2 & SR_ERRORS);
}

static int flash_unlock(void)
{
    if (FLASH_CR2 & CR_LOCK) {
        FLASH_KEYR2 = KEY1;
        FLASH_KEYR2 = KEY2;
    }
    if (FLASH_CR2 & CR_LOCK)
        return 0;
    FLASH_CCR2 = SR_ERRORS | SR_ECC | SR_EOP;
    return 1;
}

static int flash_erase(unsigned sector)
{
    if (sector >= B2_LIMIT / B2_SECTOR || !flash_wait(SPINS_WORD))
        return 0;
    FLASH_CR2 = CR_PSIZE64 | CR_SER | (sector << 8);
    FLASH_CR2 = CR_PSIZE64 | CR_SER | (sector << 8) | CR_START;
    barrier();
    int ok = flash_wait(SPINS_ERASE);
    FLASH_CR2 = CR_PSIZE64;
    return ok;
}

static int flash_word(uint32_t at, const uint32_t w[8])  /* one 256-bit flash word = eight consecutive word writes, which start the programming themselves */
{
    if (at >= B2_END || (at & 31u))
        return 0;
    volatile uint32_t *to = (volatile uint32_t *)(B2_BASE + at);
    FLASH_CR2 = CR_PSIZE64 | CR_PG;
    barrier();
    for (int i = 0; i < 8; i++)
        to[i] = w[i];
    barrier();
    int ok = flash_wait(SPINS_WORD);
    FLASH_CR2 = CR_PSIZE64;
    return ok;
}

static const uint32_t claim_word[8] = {B2_CLAIM, ~B2_CLAIM, 1u, 0, 0, 0, 0, 0};

static int claimed(void)
{
    DCIMVAC = B2_BASE + B2_CLAIM_AT;
    return *(const volatile uint32_t *)(B2_BASE + B2_CLAIM_AT) == B2_CLAIM;
}

static int blank(uint32_t from, uint32_t to)             /* byte offsets in bank 2 */
{
    const volatile uint32_t *p = (const volatile uint32_t *)B2_BASE;
    for (uint32_t i = from / 4u; i < to / 4u; i++) {
        if (p[i] != 0xffffffffu)
            return 0;
        if (!(i & 0x3fffu))
            stamp();
    }
    return 1;
}

/* What this mod's own earlier images (the stock installer wrote them whole, past the end of bank 1) left in bank 2: the cave's tail from 0x08100000, then 0xFF;
 * then either the MCL program ("MCLB" at 0x08120000, which this checks and nothing more) or 0xFF up to the ITCM image at 0x081D0000 (code, not blank). The sectors from
 * 0x081E0000 up are not looked at. A unit whose bank 2 is anything else is left alone. */
static int legacy(void)
{
    const volatile uint32_t *p = (const volatile uint32_t *)B2_BASE;
    if (p[0] == 0xffffffffu)
        return 0;                                        /* nothing at the start */
    if (p[0x20000 / 4] == 0x424c434du)                   /* "MCLB" */
        return 1;
    return blank(0x20000u, 0xD0000u) && p[0xD0000 / 4] != 0xffffffffu;
}

static int owned(void)
{
    bkp_enable();
    uint32_t was = *B2_MARK;
    if (was == B2_MARK_CLAIMING)
        return 0;                                        /* this very look never came back, in an earlier boot */
    int dead = was == B2_MARK_SCANNING;
    bkp_put(B2_MARK, B2_MARK_CLAIMING);
    int ok = claimed();
    if (!ok && !dead && (blank(0, B2_END) || legacy())) {
        ok = flash_unlock() && flash_word(B2_CLAIM_AT, claim_word);
        FLASH_CR2 = CR_PSIZE64 | CR_LOCK;
        ok = ok && claimed();
    }
    bkp_put(B2_MARK, dead ? was : 0u);                   /* a dead scan's mark stays up until the bank is rewritten */
    return ok;
}

/* Write the card file into bank 2. Only called with the pairing check saying "not paired". Nonzero when every step reported clean. */
static int b2_program(void)
{
    struct src s;
    uint32_t first[8], w[8];
    if (!src_open(&s))
        return 0;
    int ok = src_word(&s, first) && header_ok(first) && owned() && flash_unlock();
    uint32_t len = first[2];
    for (unsigned sector = 0; ok && sector * B2_SECTOR < len; sector++) {
        ok = flash_erase(sector);
        stamp();
    }
    for (uint32_t at = 32u; ok && at < len; at += 32u) {
        ok = src_word(&s, w) && flash_word(at, w);
        if (!(at & 0x1fffu))
            stamp();
    }
    if (ok)
        ok = flash_word(0, first);                       /* the header last */
    FLASH_CR2 = CR_PSIZE64 | CR_LOCK;
    fw_f_close(s.file);
    for (uint32_t a = B2_BASE; ok && a < B2_BASE + len; a += 32u)
        DCIMVAC = a;                                     /* the caches may hold the old bank 2 */
    ICIALLU = 0;
    __asm__ volatile("dsb\n isb");
    stamp();
    return ok;
}

/* How many times this payload has been programmed without a clean scan after it: twice, then leave it (a flash that cannot hold the payload would be
 * erased and rewritten on every boot for ever). A different payload starts its own count. */
static uint32_t tries_key(void)
{
    return b2_expect_sum ^ 0x54524932u;
}

static unsigned tries(void)
{
    bkp_enable();
    return *B2_TRIES_ID == tries_key() && *B2_TRIES_N <= B2_MAX_TRIES ? *B2_TRIES_N : 0u;
}

static void tries_set(unsigned n)
{
    bkp_put(B2_TRIES_ID, tries_key());
    bkp_put(B2_TRIES_N, n);
}

static void restart(void)                                /* bank 2 reads good now: start again from the top, so the machines boot with their code in place */
{
    __asm__ volatile("dsb");
    *(volatile uint32_t *)0xe000ed0cu = (0x5fau << 16) | (1u << 2);      /* AIRCR: SYSRESETREQ */
    for (;;)
        ;
}

/* b2load_thunk.S calls this from the disk task, in front of a file command step. It works once a boot, the first time stock has found a card:
 * B2_LOOKED goes up BEFORE anything else, so an attempt that dies half way is not repeated until the next power-on. */
void b2_load_poll(void)
{
    if ((B2_STATE & B2_LOOKED) || CARD_FOUND != 1u)
        return;
    int live = b2_gate();
    B2_STATE |= B2_LOOKED;
    unsigned verdict = file_verdict();
    B2_STATE |= verdict;
    if (live) {
        owned();                 /* paired: it is the mod's beyond doubt; a unit whose payload predates the claim gets its claim now, while the bank reads well */
    } else if (verdict == B2_FILE_OK && !owned()) {
        B2_STATE |= B2_FOREIGN;  /* not blank and not ours: not this code's to erase */
    } else if (verdict == B2_FILE_OK) {
        unsigned n = tries();
        if (n < B2_MAX_TRIES) {
            tries_set(n + 1u);   /* before the write, so one that kills the boot still counts */
            if (b2_program() && b2_rescan()) {
                tries_set(0);
                restart();
            }
        }
    }
}
