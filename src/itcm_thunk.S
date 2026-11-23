/* Flash entries for the functions that run from ITCM (src/cave.ld) and are called straight from stock code: a hook's BL / B.W reaches
 * only +-16 MB, ITCM is at 0x00000000. Each entry jumps on with every register as it was (LDR to PC switches to the Thumb target). */
    .syntax unified
    .thumb
.macro FLASH_ENTRY name
    .section .text.\name\()_flash, "ax", %progbits
    .global \name\()_flash
    .thumb_func
    .align 2
\name\()_flash:
    ldr.w   pc, 1f
    .align 2
1:  .word   \name
.endm
    FLASH_ENTRY comp_process
    FLASH_ENTRY looper_in
    FLASH_ENTRY munchi_process
    FLASH_ENTRY od_process
