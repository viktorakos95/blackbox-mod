/* The Blackbox platform layer for MCL (milestone M8): locks, soft interrupts, time, input. Included by platform.h (which MCL's sources include). */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* masks priorities 3 and lower urgency (the soft interrupts, audio, the kernel); the clock (1) and the MIDI jack (0) keep running */
static inline uint32_t bb_lock(void)
{
    uint32_t old;
    __asm volatile("mrs %0, basepri" : "=r"(old));
    if (old == 0 || old > 0x30u)
        __asm volatile("msr basepri, %0" ::"r"(0x30u) : "memory");
    return old;
}

static inline void bb_unlock(uint32_t old) { __asm volatile("msr basepri, %0" ::"r"(old) : "memory"); }

static inline int bb_in_interrupt(void)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    return ipsr != 0;
}

void bb_swirq_pend(int n);                      /* pends soft interrupt 1 (sequencer), 2 (MIDI input), 3 (GUI poll) */

/* the panel, from the MCL page (bb_ui / src/cpu.c): see bb_mcl.cpp */
void bb_mcl_input_button(uint32_t id, int down);       /* MCL button id (ButtonsClass::BUTTON1 ...) */
void bb_mcl_input_encoder(uint32_t n, int detents);    /* encoder n turned by detents */

#ifdef __cplusplus
}
#endif
