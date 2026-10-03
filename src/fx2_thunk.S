/*
 * The reverb process method's core call (bl FUN_08074f50 @0x0806318e): r0 state, r1 in, r2 outL, r3 outR, [sp] n.
 * The object (r4 there) is needed to find which reverb cell this is, so park it, then fall into fx2_core with the
 * call's own registers and stack untouched.
 */
    .syntax unified
    .thumb
    .text

    .global fx2_core_thunk
    .thumb_func
    .type fx2_core_thunk, %function
fx2_core_thunk:
    ldr     r12, =0x38800b00
    str     r4, [r12]
    b.w     fx2_core
    .ltorg
