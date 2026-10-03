/*
 * Voice-start thunks: called in place of the two pad classes' voice-start functions. If the pad object is pad 1
 * (id 0 at +0x18) set the duck trigger flag, then continue into the original with every argument untouched.
 */
    .syntax unified
    .thumb
    .text

    .equ DUCK_TRIG, 0x2405ff70 + 8

    .macro voice_start name, original
    .global \name
    .thumb_func
    .type \name, %function
\name:
    ldr     r12, [r0, #0x18]
    cmp     r12, #0
    bne     1f
    push    {r0, r1}
    ldr     r1, =DUCK_TRIG
    movs    r0, #1
    strb    r0, [r1]
    pop     {r0, r1}
1:  ldr     r12, =\original + 1
    bx      r12
    .ltorg
    .endm

    voice_start duck_voice_start_a, 0x08058548
    voice_start duck_voice_start_b, 0x08065908
