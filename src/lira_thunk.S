/*
 * Entry points for the firmware's settings-store getter (0x08093e9c) and setter (0x08093ecc), whose first four bytes
 * become B.W to these (patches/lira.py). The stock functions leave r1 alone, so callers may keep a value there:
 * the thunks save it around the C functions.
 */
    .syntax unified
    .thumb
    .text

    .global lira_get
    .thumb_func
    .type lira_get, %function
lira_get:
    push    {r1, lr}
    bl      lira_get_c
    pop     {r1, pc}

    .global lira_set
    .thumb_func
    .type lira_set, %function
lira_set:
    push    {r1, lr}
    bl      lira_set_c
    pop     {r1, pc}

/*
 * Replaces the two calls of the audio task's input conversion (FUN_0804caa4(engine, rx buffer, words): interleaved int32
 * -> the engine's left / right float buffers at engine + 0x8fb0 / + 0x8fb4): remember the engine object and the frame
 * count in backup SRAM (0x38800bf8 / 0x38800bfc) for LIRA's Ext In, then carry on into the stock function.
 */
    .global lira_in_capture
    .thumb_func
    .type lira_in_capture, %function
lira_in_capture:
    ldr     ip, =0x38800bf8
    str     r0, [ip]
    lsrs    r3, r2, #1
    str     r3, [ip, #4]
    b.w     0x0804caa4
    .ltorg
