/* Code cave: appended directly after the stock image (which ends at 0x080F1E78). Bank 1 ends at 0x08100000; the size test
 * (step 40) showed the updater writes and the unit reads past it, so the cave may run on into bank 2. The MCL pad test
 * (patches/mclpad.py) read back a pattern up to 0x08180000 (POK on the unit), so the window is proven to 0x08180000: the main cave ends at 0x08120000 and the MCL blob owns the rest (0x081E0000 and up stay
 * untouched: something may live there). */
MEMORY { CAVE (rx) : ORIGIN = 0x080F1E78, LENGTH = 0x2E188 }      /* to 0x08120000; 0x08120000..0x08180000 is the MCL blob (src/mcl_bb/mcl_blob.ld), proven by the pad test (POK) */
SECTIONS {
  .cave : { *(.text .text.* .rodata .rodata.*) } > CAVE
  /DISCARD/ : { *(.data .data.* .bss .bss.* COMMON .ARM.exidx* .comment) }
}
