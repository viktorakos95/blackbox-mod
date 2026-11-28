/*
 * Flash bank 2, the bank 1 side. Prior art: j3threejay/blackbox-mod (MIT, THIRD_PARTY.md): the pairing check and the card loader are
 * their design, rewritten here for a payload that holds CODE the hooks jump into (the machines, the MCL program), not leaf DSP.
 *
 * WHY. On some units the stock SD installer only writes flash bank 1 (up to 0x08100000): an image that runs past it is "unable to verify".
 * So BLACKBOX.bin holds the core features and stops short of 0x08100000 (the cave region of src/cave.ld), and the machines, MCL and their
 * ITCM code are a second file, BBEXTRA.BIN, which b2load.c programs into bank 2 itself from the card (the first boot that finds a mismatch).
 *
 * PAYLOAD (what BBEXTRA.BIN holds, and bank 2 from 0x08100000), header = one 32-byte flash word:
 *   0 magic "BB2!"   4 version (1)   8 length of the payload   12 sum of its 32-bit words from byte 32 to the end
 *   16 offset of the machines' ITCM image   20 its length   24 its address in ITCM   28 0
 *   32.. machine code (src/cave_b2.ld), the MCL blob at 0x20000 (when there is one), the ITCM image (anywhere after, the header says where)
 * Bank 1 carries the sum of the payload it was built for (b2_expect_sum, patched in by patch.py): any other payload is not "paired".
 *
 * WHAT RUNS WHEN: every hook that jumps into a bank 2 function goes through a bank 1 stub (patches/b2guard.py, src/b2stubs.S) that asks b2_gate();
 * unpaired, the stub runs what stock would have run, so no code in bank 2 is reached. The first b2_gate() decides (b2.c).
 */
#ifndef B2_H
#define B2_H
#include <stdint.h>

#define B2_MAGIC   0x21324242u   /* "BB2!" */
#define B2_BASE    0x08100000u
#define B2_LIMIT   0x000C0000u   /* sectors 0-5: where a payload may be, and all the loader ever erases */
#define B2_CLAIM_AT B2_LIMIT     /* sector 6, first flash word: "this bank is the mod's" (b2load.c). Never erased. */
#define B2_END     0x000E0000u   /* the end of sector 6. SECTOR 7 (0x081E0000) IS THE UNIT'S FACTORY CALIBRATION: never touched */
#define B2_SECTOR  0x00020000u
#define B2_CLAIM   0x43324242u   /* "BB2C" */
#define B2_HDR     32u

/* the pairing verdict: ITCM 0x100 (the first 512 bytes of ITCM are left empty by src/cave.ld; b2_boot clears it each boot) */
static inline volatile uint32_t *b2_itcm(uint32_t a)    /* (ITCM is near address 0: the compiler must not reason about a null pointer) */
{
    __asm__ volatile("" : "+r"(a));
    return (volatile uint32_t *)a;
}
#define B2_VERDICT   (*b2_itcm(0x100u))
#define B2_PAIRED    0x42325041u
#define B2_UNPAIRED  0x4232554eu
/* state bits (ITCM 0x104) */
#define B2_STATE     (*b2_itcm(0x104u))
#define B2_LOOKED    0x01u       /* the card loader has had its one go this boot */
#define B2_FOREIGN   0x02u       /* bank 2 holds something that is neither blank nor the mod's: the loader left it alone */
#define B2_FILE_OK   0x10u       /* the card has this image's own payload, whole */
#define B2_FILE_NONE 0x20u
#define B2_FILE_WRONG 0x30u

/* backup SRAM: 0x38800394..0x388003f0 is free (od.c uses to 0x384, crash.c 0x390, crash.c's frozen shares from 0x3f0) */
#define B2_MARK          ((volatile uint32_t *)0x388003c8u)
#define B2_TRIES_ID      ((volatile uint32_t *)0x388003ccu)
#define B2_TRIES_N       ((volatile uint32_t *)0x388003d0u)
#define B2_MARK_SCANNING 0x4e435332u         /* "2SCN": the pairing scan is reading bank 2; still set at boot = it never came back (a bad flash word) */
#define B2_MARK_CLAIMING 0x4d4c4332u         /* "2CLM": the loader is looking the bank over, same idea */
#define B2_MAX_TRIES     2u

void bkp_enable(void);           /* od.c */
static inline void bkp_put(volatile uint32_t *at, uint32_t value)
{
    *at = value;
    *(volatile uint32_t *)0xe000ef68u = (uint32_t)at;    /* DCCMVAC: write this cache line out to the SRAM (it holds a write back, a reset does not flush it) */
    __asm__ volatile("dsb\n isb");
}

extern const volatile uint32_t b2_expect_sum;   /* b2.c: the sum of the payload this image is built for (patch.py) */

int b2_gate(void);               /* 1 = bank 2 is this image's payload, read clean: its code may run. Decides on the first call. */
void b2_boot(void);              /* cpu_boot: forget last boot's verdict */
int b2_rescan(void);             /* the loader has rewritten bank 2: decide again; 1 = paired */
void b2_load_poll(void);         /* b2load.c, from the disk task's loop */

#endif
