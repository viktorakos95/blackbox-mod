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

/*
 * Drawing guard for the Looper page (looper_draw_blocked, solo.c): while the page shows full screen, everything the
 * stock firmware draws is dropped, so the screen's own top bar and the mixer cells' widgets cannot show through or on
 * top of the page. Each stub replaces the first instructions of one low-level drawing primitive, asks
 * looper_draw_blocked, and either returns at once or runs the displaced instructions and continues the primitive.
 * Hooks: 0x0808f920 rectangle fill, 0x0808f524 pixel, 0x0808f624, 0x0808f4a4, 0x0808f740 (the other line / blit forms).
 */
    .macro GUARD name, resume
    .global \name
    .thumb_func
    .type \name, %function
\name:
    push    {r0, r1, r2, r3, r12, lr}     /* 24 bytes: the stack stays 8-byte aligned for the C call */
    bl      looper_fill_blocked
    str     r0, [sp, #16]                   /* the answer comes back in r12's slot */
    pop     {r0, r1, r2, r3, r12, lr}
    cmp     r12, #0
    bne     9f
    .endm

    .macro GUARD_END resume
    ldr     r12, =\resume
    bx      r12
9:  bx      lr
    .ltorg
    .endm

    GUARD   guard_fill, 0x0808f925
    push    {r4, r5, lr}
    sub     sp, #0x14
    GUARD_END 0x0808f925

    GUARD   guard_pixel, 0x0808f529
    push    {r4, r5, r6, lr}
    mov     r4, r0
    GUARD_END 0x0808f529

    GUARD   guard_f624, 0x0808f629
    push    {r4, r5, r6, r7, lr}
    mov     r6, r2
    GUARD_END 0x0808f629

    GUARD   guard_f4a4, 0x0808f4a9
    push    {r3, lr}
    mov     r2, r1
    GUARD_END 0x0808f4a9

    GUARD   guard_f740, 0x0808f747
    push    {r4, lr}
    ldrb.w  lr, [sp, #8]
    GUARD_END 0x0808f747

/*
 * Stock line and text drawing, dropped while the Looper page shows (looper_stock_blocked, solo.c): the stock cells' corner
 * marks (the cyan boxes: FUN_080a43b8 draws them with FUN_0808ea7e, colour 5) and the screen's own "1:2 Seq" bar (text, FUN_0808ee54).
 * The page draws with fills and pixels only. Both functions start with `push.w {r4-r8, sb, sl, fp, lr}`.
 */
    .macro STOCKGUARD name, resume
    .global \name
    .thumb_func
    .type \name, %function
\name:
    push    {r0, r1, r2, r3, r12, lr}
    bl      looper_stock_blocked
    str     r0, [sp, #16]
    pop     {r0, r1, r2, r3, r12, lr}
    cmp     r12, #0
    bne     9f
    push.w  {r4, r5, r6, r7, r8, sb, sl, fp, lr}
    ldr     r12, =\resume
    bx      r12
9:  movs    r0, #0
    bx      lr
    .ltorg
    .endm

    STOCKGUARD guard_line, 0x0808ea83
    STOCKGUARD guard_text, 0x0808ee59

/*
 * Diagnostic: the engine's event post (FUN_0804f504(list, event), 24-byte events queued for the audio task; the
 * transport buttons must end up here). Calls looper_note_event(list, event) first, then runs the displaced
 * `ldr.w r3, [r0, #0x600]` and continues at 0x0804f508.
 */
    .global guard_post
    .thumb_func
    .type guard_post, %function
guard_post:
    push    {r0, r1, r2, r3, r12, lr}
    bl      looper_note_event
    str     r0, [sp, #16]
    pop     {r0, r1, r2, r3, r12, lr}
    cmp     r12, #0
    bne     9f
    ldr.w   r3, [r0, #0x600]
    ldr     r12, =0x0804f509
    bx      r12
9:  bx      lr
    .ltorg
