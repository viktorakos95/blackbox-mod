/*
 * Trigger2MIDI's view of the stock pad hits: in place of the first two instructions of FUN_0804c61c (the engine note-on
 * the pads and the Keys screen call: cmp r2, #0x7f / bls), jump here. Tell t2m_learn (engine, pad, note, a4) what is
 * being played (it only acts while LEARN waits), then do what those two instructions did and go on into the function.
 */
    .syntax unified
    .thumb
    .text

    .global t2m_note_thunk
    .thumb_func
    .type t2m_note_thunk, %function
t2m_note_thunk:
    push    {r0, r1, r2, r3, r12, lr}
    bl      t2m_learn
    pop     {r0, r1, r2, r3, r12, lr}
    cmp     r2, #0x7f
    bhi     1f
    ldr     r12, =0x0804c622 + 1
    bx      r12
1:  bx      lr
    .ltorg
