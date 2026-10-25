/* The crash recorder (crash.c): why the unit last restarted, kept in the backup SRAM (which a reset does not clear).
 * Breadcrumbs: each task writes where it is in our code, so that a restart with no fault (the watchdog: a hang, or the
 * stock allocator halting when it runs out) still says what was running. 0 = not in our code (stock, or idle). */
#ifndef CRASH_H
#define CRASH_H
#include <stdint.h>

enum { BC_AUDIO, BC_GUI, BC_FILE, BC_N };
enum {
    BCA_STAGE = 1, BCA_DRONE, BCA_SAMPLR, BCA_TRACKS, BCA_RECFILL, BCA_IN,
    BCG_MSG = 10, BCG_PAINT, BCG_TOUCH, BCG_SCREEN,
    BCF_SAVE = 20, BCF_LOAD,
};
#define CRASH_BC ((volatile uint8_t *)0x38800b24u)          /* the live breadcrumbs (crash.c's live block) */
static inline void crash_bc(int task, int id)
{
    CRASH_BC[task] = (uint8_t)id;
}

void crash_boot(void);                          /* from looper_boot: the reset's cause, the last run's record */
void crash_tick(void);                          /* from the audio stage, once a block: uptime, free RAM low point */
int crash_line(int i, char *buf, int n);        /* the debug view's line i (0..); 0 when there are no more */
void crash_clear(void);
int crash_loading(void);                        /* a long load: the stock loader has read the card a lot for two seconds and still is */
uint32_t crash_irq_off(void);                  /* interrupts off: the old PRIMASK */
void crash_irq_on(uint32_t v);                 /* ... and back */
int crash_pending(void);                        /* a record of a restart not looked at yet */

#endif
