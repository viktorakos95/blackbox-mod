/* Code cave: appended directly after the stock image (which ends at 0x080F1E78), up to the end of flash bank 1
 * (0x08100000). Sector 7 (0x080E0000-0x080FFFFF) already holds the end of the image, so the updater erases and
 * writes it whole; nothing else lives there. */
MEMORY { CAVE (rx) : ORIGIN = 0x080F1E78, LENGTH = 0xE188 }
SECTIONS {
  .cave : { *(.text .text.* .rodata .rodata.*) } > CAVE
  /DISCARD/ : { *(.data .data.* .bss .bss.* COMMON .ARM.exidx* .comment) }
}
