/* Code cave: appended directly after the stock image (which ends at 0x080F1E78). Bank 1 ends at 0x08100000; the size test
 * (step 40) showed the updater writes and the unit reads past it, so the cave may run on into bank 2 up to 0x08140000
 * (0x081E0000 and up stay untouched: something may live there). */
MEMORY { CAVE (rx) : ORIGIN = 0x080F1E78, LENGTH = 0x4E188 }      /* to 0x08140000: the updater writes flash bank 2 (tested: B2 OK) */
SECTIONS {
  .cave : { *(.text .text.* .rodata .rodata.*) } > CAVE
  /DISCARD/ : { *(.data .data.* .bss .bss.* COMMON .ARM.exidx* .comment) }
}
