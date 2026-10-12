/* Code cave: appended directly after the stock image (which ends at 0x080F1E78). */
/* Up to the end of the stock image's last flash sector (0x080E0000..0x080FFFFF), so the image stays in bank 1. */
MEMORY { CAVE (rx) : ORIGIN = 0x080F1E78, LENGTH = 0x08100000 - 0x080F1E78 }
SECTIONS {
  .cave : { *(.text .text.* .rodata .rodata.*) } > CAVE
  /DISCARD/ : { *(.data .data.* .bss .bss.* COMMON .ARM.exidx* .comment) }
}
