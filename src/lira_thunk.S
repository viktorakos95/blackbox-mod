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
