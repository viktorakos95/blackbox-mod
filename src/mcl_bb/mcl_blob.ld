/* MCL blob (milestone M2). Code and read-only data run in place from flash bank 2 at 0x08120000..0x08180000 (proven window: the
 * pad test read back POK up to 0x08180000); the main cave (src/cave.ld) must end below 0x08120000.
 * .data/.bss/heap live in a fixed SDRAM region at the top of the 64 MB, reserved by lowering the allocator's initial top
 * (patches/mclblob.py rewrites the .data image word at 0x080f1e08: 0xc4000000 -> 0xc3f80000), so every address is known at link time. */
MEMORY {
  BLOB (rx)  : ORIGIN = 0x08120000, LENGTH = 0x60000
  RAM  (rwx) : ORIGIN = 0xC3F80000, LENGTH = 0x80000
}
ENTRY(mcl_entry_table)
SECTIONS {
  .entry : { KEEP(*(.entry)) } > BLOB
  .text : { *(.text .text.* .rodata .rodata.* .glue_7 .glue_7t .vfp11_veneer) } > BLOB
  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > BLOB
  .init_array : { . = ALIGN(4); __init_array_start = .; KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) __init_array_end = .; } > BLOB
  .data : { . = ALIGN(4); __data_start = .; *(.data .data.*) . = ALIGN(4); __data_end = .; } > RAM AT > BLOB
  __data_lma = LOADADDR(.data);
  .bss (NOLOAD) : { . = ALIGN(16); __bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(16); __bss_end = .; } > RAM
  . = ALIGN(16);
  __heap_start = .;
  __heap_limit = ORIGIN(RAM) + LENGTH(RAM);
  /DISCARD/ : { *(.ARM.exidx* .comment .note* .eh_frame*) }
}
