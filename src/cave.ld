/* Code cave: appended directly after the stock image (which ends at 0x080F1E78). */
MEMORY { CAVE (rx) : ORIGIN = 0x080F1E78, LENGTH = 0x8000 }
SECTIONS {
  .cave : { *(.text .text.* .rodata .rodata.*) } > CAVE
  /DISCARD/ : { *(.data .data.* .bss .bss.* COMMON .ARM.exidx* .comment) }
}
