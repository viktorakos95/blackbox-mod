/*
 * Replaces the engine's `bl rand` in the sequence player's "roll for this note" branch. The caller then plays the
 * note when (r0 % 100) <= the event's cond byte, which it reloads from its own stack frame at [sp, #0x31].
 * cond 1..99: hand over to the stock rand(). cond >= 100 (A:B): ask cond_decide; to skip, zero the caller's copy
 * of cond and return 1, to play return 0. r5 = the sequence object in the caller.
 */
    .syntax unified
    .thumb
    .text

    .equ EVENT_COND, 0x31

    .global cond_roll
    .thumb_func
    .type cond_roll, %function
cond_roll:
    ldrb.w  r0, [sp, #EVENT_COND]
    cmp     r0, #100
    bhs     1f
    ldr     r12, =0x080c89d4 + 1
    bx      r12
1:  push    {r4, lr}
    mov     r1, r5
    bl      cond_decide
    pop     {r4, lr}
    cbz     r0, 2f
    movs    r0, #0
    bx      lr
2:  strb.w  r0, [sp, #EVENT_COND]
    movs    r0, #1
    bx      lr
    .ltorg
