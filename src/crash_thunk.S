/*
 * The crash recorder's entries (crash.c).
 * crash_fault: the hard / memory / bus / usage fault vectors (patches/crash.py). Finds the exception frame (MSP or PSP,
 * from EXC_RETURN bit 2), carries on 128 bytes below that frame (so the frame and what is above it stay as they were; a
 * frame outside the RAM means a broken stack: then the top of the DTCM) and calls crash_record(frame, EXC_RETURN, IPSR),
 * which never returns.
 * crash_oom_thunk: in place of the stock allocator's message call just before it halts (@0x08044452). r0 = the message,
 * r3 = the size asked for (rounded up to 16), [sp, #4] = the allocator's caller (its pushed lr). Calls crash_oom(msg, size, caller).
 */
    .syntax unified
    .thumb
    .text

    .global crash_fault
    .thumb_func
    .type crash_fault, %function
crash_fault:
    cpsid   i
    tst     lr, #4
    ite     eq
    mrseq   r0, msp
    mrsne   r0, psp
    mov     r1, lr
    mrs     r2, ipsr
    sub     r3, r0, #128
    bic     r3, r3, #7
    cmp     r0, #0x20000000
    blo     2f
    ldr     r12, =0x38010000
    cmp     r0, r12
    blo     3f
2:  ldr     r3, =0x2001fff0
3:  mov     sp, r3
    bl      crash_record
1:  b       1b

    .global crash_oom_thunk
    .thumb_func
    .type crash_oom_thunk, %function
crash_oom_thunk:
    ldr     r2, [sp, #4]
    mov     r1, r3
    b       crash_oom
