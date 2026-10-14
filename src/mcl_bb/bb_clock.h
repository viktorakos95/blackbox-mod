/* MCL on the Blackbox: the MIDI clock source (milestones M4, M5). TIM6 interrupt at 192 PPQN, 24 PPQN MIDI clock out through MCL's TX ring. */
#pragma once
#include <stdint.h>

extern "C" {
void bb_clock_down(void);                      /* first thing in mcl_boot: clock stopped, state cleared */
int bb_clock_init(void);                       /* enable TIM6, measure its clock against the cycle counter (~2 ms, GUI task only). 1 = ok */

/* internal tempo (M4) */
void bb_clock_start(uint32_t bpm_x10, int send_start);      /* tempo in 1/10 BPM (20.0 .. 300.0); send_start: FA first, aligned to a tick */
void bb_clock_stop(int send_stop);             /* FC; the timer stops (also ends MIDI-in sync) */
void bb_clock_tempo(uint32_t bpm_x10);         /* takes effect at the next tick; no jump of the phase */

/* follow the MIDI clock arriving on the jack (M5): the timer locks to the F8 arrival times, so the clock we send (and MCL's 192-PPQN tick) is the input's, smoothed.
 * FA / FB / FC from the jack are forwarded in step. Call after bb_clock_init(); bb_clock_stop() ends it. */
void bb_clock_sync_midi(void);
void bb_clock_in_rt(uint8_t byte, uint32_t cycles);   /* from the receive tap (interrupt context): a real-time byte F8..FC and its arrival time */

int bb_clock_running(void);                    /* the clock is being sent (internal, or MIDI-in sync locked) */
uint32_t bb_clock_ticks(void);                 /* 192-PPQN ticks since start (reset to 0 by a received FA in sync mode) */
uint32_t bb_clock_timer_hz(void);              /* the measured TIM6 count rate (after its prescaler) */
uint32_t bb_clock_isr_calls(void);             /* entries of the timer interrupt since boot, counted even when the clock is off */
void bb_clock_set_hook(void (*tick)(uint32_t ticks192));   /* called from the timer interrupt on every 192-PPQN tick (MCL's sequencer trigger) */

/* sync status */
int bb_clock_sync_state(void);                 /* 0 internal, 1 listening for the first clock, 2 saw one clock, 3 locked and sending, 4 input lost (flywheel) */
uint32_t bb_clock_bpm_x10(void);               /* the tempo being sent, 1/10 BPM */
uint32_t bb_clock_err_us(int which);           /* 0: largest |phase error| of the last second of clocks, 1: of the last clock, microseconds; 2: clocks processed */
}
