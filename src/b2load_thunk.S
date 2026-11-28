/*
 * The card loader's hook (see b2load.c). The disk task's loop FUN_080743b0 calls stock's file command step FUN_08092f5c from three places
 * (0x0807443e, 0x0807446e, 0x080744d8); each `bl` comes here instead, the loader has its look, and the step runs as it would have.
 * r0 is the step's argument; r1-r3 are handed on untouched. (patches/lirasave.py hooks two of those places for the machines: when bank 2 is not
 * paired, patches/b2guard.py's stub for them ends here too.)
 */
    .syntax unified
    .thumb
    .text

    .global b2load_thunk
    .thumb_func
    .type b2load_thunk, %function
b2load_thunk:
    push    {r0, r1, r2, r3, r12, lr}
    bl      b2_load_poll
    pop     {r0, r1, r2, r3, r12, lr}
    ldr     r12, =0x08092f5d
    bx      r12
    .ltorg
