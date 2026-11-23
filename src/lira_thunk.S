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

/* Saving (lira_pad.c): the preset queue's save handler calls 0x08091a5c (r0 = the preset manager, r1 = ...) at 0x080930ce, the load
   handler 0x08091a74 at 0x080930e0: the stock function, then our file */
    .global lira_save_thunk
    .type   lira_save_thunk, %function
lira_save_thunk:
    push    {r4, lr}
    ldr     r2, =0x08091a5d
    blx     r2
    bl      lira_save
    pop     {r4, pc}
    .global lira_load_thunk
    .type   lira_load_thunk, %function
lira_load_thunk:
    push    {r4, lr}
    ldr     r2, =0x08091a75
    blx     r2
    bl      lira_load
    pop     {r4, pc}
    .ltorg

/* Recording onto a pad (lira_pad.c): the recorder's param 0x9a store at 0x08062c42 (r3 = the value, r4 = the node) */
    .global lira_rec_store_thunk
    .type   lira_rec_store_thunk, %function
lira_rec_store_thunk:
    push    {r0, r1, r2, ip, lr}
    mov     r0, r3
    bl      lira_rec_store
    mov     r3, r0
    pop     {r0, r1, r2, ip, lr}
    strb.w  r3, [r4, #0x58]
    bx      lr

/* ... and its Resam path's copy of the master bus (0x08062784, bl 0x0804d880 (dst r0, src r1)): the stock copy, then ours over it */
    .global lira_rec_copy_thunk
    .type   lira_rec_copy_thunk, %function
lira_rec_copy_thunk:
    push    {r0, r4, lr}
    ldr     r2, =0x0804d881
    blx     r2
    ldr     r0, [sp]
    bl      lira_rec_fill
    pop     {r0, r4, pc}
    .ltorg
