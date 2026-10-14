/* MCL on the Blackbox: the MIDI clock source (milestone M4). TIM6 interrupt at 192 PPQN, 24 PPQN MIDI clock out through MCL's TX ring. */
#pragma once
#include <stdint.h>

extern "C" {
void bb_clock_down(void);                      /* first thing in mcl_boot: clock stopped, state cleared */
int bb_clock_init(void);                       /* enable TIM6, measure its clock against the cycle counter (~2 ms, GUI task only). 1 = ok */
void bb_clock_start(uint32_t bpm_x10, int send_start);      /* tempo in 1/10 BPM (20.0 .. 300.0); send_start: FA first, aligned to a tick */
void bb_clock_stop(int send_stop);             /* FC */
void bb_clock_tempo(uint32_t bpm_x10);         /* takes effect at the next tick; no jump of the phase */
int bb_clock_running(void);
uint32_t bb_clock_ticks(void);                 /* 192-PPQN ticks since start */
uint32_t bb_clock_timer_hz(void);              /* the measured TIM6 count rate (after its prescaler) */
void bb_clock_set_hook(void (*tick)(uint32_t ticks192));   /* called from the timer interrupt on every 192-PPQN tick (MCL's sequencer trigger, M5) */
}
