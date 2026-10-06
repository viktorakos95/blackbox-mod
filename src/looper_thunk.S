/*
 * Out 1 mix point for the live looper. Called in place of `ldrb.w r3, [r5, #0xd60]` (@0x08053528), the "is the
 * master compressor on?" test just before the compressor stage call (@0x08053ca0, obj = fp + 0xfc40, buffers = [sp]).
 * That point runs every block whether the compressor is on or off, and before it and the output levels.
 * Adds the loop to the Out 1 bus (looper_stage, looper.c), then performs the replaced load and returns.
 * The instruction before it is an indirect call, so no caller-saved register is live here; r0-r2 and r12 are kept
 * anyway.
 */
    .syntax unified
    .thumb
    .text

    .global looper_stage_thunk
    .thumb_func
    .type looper_stage_thunk, %function
looper_stage_thunk:
    push    {r0, r1, r2, r3, r12, lr}
    movw    r0, #0xfc40
    add     r0, r0, fp
    ldr     r1, [sp, #24]           /* the caller's [sp]: the engine's buffer set */
    bl      looper_stage
    pop     {r0, r1, r2, r3, r12, lr}
    ldrb.w  r3, [r5, #0xd60]
    bx      lr
