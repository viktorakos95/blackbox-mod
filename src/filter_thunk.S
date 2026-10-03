/*
 * Per-voice filter call sites. r0 = filter object, r1 = audio buffer; r2 = the pad object the voice belongs to.
 * Sample pads (class A, 0x08055c60): the pad object is in r6.
 * Clip / slicer pads (class B, 0x080669d6): one filter object per pad, at pad + 0x3ac.
 */
    .syntax unified
    .thumb
    .text

    .global filter_voice_a
    .thumb_func
    .type filter_voice_a, %function
filter_voice_a:
    mov     r2, r6
    b.w     filter_process

    .global filter_voice_b
    .thumb_func
    .type filter_voice_b, %function
filter_voice_b:
    sub.w   r2, r0, #0x3ac
    b.w     filter_process

