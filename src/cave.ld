/* Code cave: appended directly after the stock image (which ends at 0x080F1E78). Bank 1 ends at 0x08100000; the size test
 * (step 40) showed the updater writes and the unit reads past it, so the cave may run on into bank 2. The MCL pad test
 * (patches/mclpad.py) read back a pattern up to 0x08180000 (POK on the unit), so the window is proven to 0x08180000: the main cave ends at 0x08120000 and the MCL blob owns the rest (0x081E0000 and up stay
 * untouched: something may live there).
 *
 * ITCM: the core's 64 KB of zero-wait code memory at 0x00000000. The stock firmware never uses it (the TCM probe, build 89:
 * "TCM I0"), so the audio code that runs every sample is linked to run there (no flash wait states, no instruction cache
 * misses). Its image is stored in flash at 0x08170000 (the top 64 KB of the MCL window: the blob must stay below it) and
 * looper_boot copies it in (src/looper.c itcm_load) before any engine is set up. Calls between flash and ITCM go through
 * the linker's long-branch veneers. */
MEMORY {
  CAVE  (rx)  : ORIGIN = 0x080F1E78, LENGTH = 0x2E188      /* to 0x08120000; 0x08120000..0x08170000 is the MCL blob (src/mcl_bb/mcl_blob.ld) */
  ITCML (rx)  : ORIGIN = 0x08170000, LENGTH = 0x10000      /* the ITCM image, as stored */
  ITCM  (rwx) : ORIGIN = 0x00000200, LENGTH = 0xFE00       /* (the first 512 bytes left empty: a call through a null pointer lands in nothing of ours) */
}
SECTIONS {
  .itcm : {
    __itcm_start = .;
    *(.text.lira_process .text.fexp2*)
    *(.text.samplr_run .text.sample_block_* .text.grain_spawn*)
    *(.text.play_seg* .text.looper_filter .text.looper_in .text.svf .text.soft .text.crunch .text.frame_at .text.rd_span .text.lpos)
    *(.text.munchi_process .text._ZN13granularDelay12processDelayEPfS0_ .text._ZN10delayVoice4ReadEPfS0_)
    *(.text.filter_process .text.comp_process .text.od_process)
    . = ALIGN(4);
    __itcm_end = .;
  } > ITCM AT> ITCML
  __itcm_load = LOADADDR(.itcm);
  .cave : { *(.text .text.* .rodata .rodata.*) } > CAVE
  /DISCARD/ : { *(.data .data.* .bss .bss.* COMMON .ARM.exidx* .comment) }
}
