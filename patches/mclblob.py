"""MCL blob (milestone M2). Apply AFTER `cave`:  python3 patch.py ... cave mclblob     (build the blob first: tools/mcl/build_blob.sh)

Image layout: stock, main cave (src/cave.ld, ends below 0x08120000), 0xFF fill, the blob at 0x08120000 (code, read-only data, the .data
image; src/mcl_bb/mcl_blob.ld). Two more bytes of the stock image change:
  - the SDRAM allocator's initial top in the .data image (0x080f1e08, copied to 0x24000088 at reset): 0xc4000000 -> 0xc3f80000, which
    reserves 0xc3f80000..0xc4000000 (512 KB) for the blob's .data/.bss/heap at link-time addresses. Everything the firmware allocates
    top-down now stays below it; the free SDRAM gap at boot shrinks by 512 KB;
  - `mcl_entry` in the main cave (src/cpu.c) is set to the blob's entry table, so cpu.c start() boots the blob once per power-up.
  - M3, the MIDI jack (src/mcl_bb/bb_midi.cpp), three hooks, all no-ops until the blob has booted:
      0x080446d4  the one call to the stock ring push (0x080444f4) inside the receive path -> mcl_rx_tap (copies the byte, then calls stock);
      0x08044dfc  entry of the stock send function (port, buf, len) -> mcl_tx_hook (stock code unless MCL has claimed the port);
      0x0804015c  vector 71 (USART6): 0x080462e5 -> mcl_usart6_isr (sends MCL's TX bytes, passes everything else to the stock handler).
Not combined with `mclpad` (the pad test covers the same flash range).
"""
import os

import struct

from thumb import ROOT, b_w, bl, symbols

STOCK_END, BLOB_ORG, RAM_ORG = 0x080F1E78, 0x08120000, 0xC3F80000

cave = open(os.path.join(ROOT, "out", "cave.bin"), "rb").read()
blob = open(os.path.join(ROOT, "out", "mcl.bin"), "rb").read()
csym = symbols(os.path.join(ROOT, "out", "cave.elf"))
bsym = symbols(os.path.join(ROOT, "out", "mcl.elf"))
end = STOCK_END + len(cave)
assert end <= BLOB_ORG, f"main cave ends at {end:#x}, past the blob start {BLOB_ORG:#x}"
assert bsym["mcl_entry_table"] == BLOB_ORG
assert BLOB_ORG + len(blob) <= 0x08180000, "blob past the proven flash window"

PATCHES = [
    (0x080F1E08, (0xC4000000).to_bytes(4, "little"), RAM_ORG.to_bytes(4, "little")),
    (csym["mcl_entry"], bytes(4), BLOB_ORG.to_bytes(4, "little")),
    (0x080446D4, bl(0x080446D4, 0x080444F4), bl(0x080446D4, bsym["mcl_rx_tap"])),
    (0x08044DFC, bytes.fromhex("012801d9"), b_w(0x08044DFC, bsym["mcl_tx_hook"])),            # cmp r0,#1 ; bls (port check, redone in the hook)
    (0x0804015C, struct.pack("<I", 0x080462E5), struct.pack("<I", bsym["mcl_usart6_isr"] | 1)),
    (end, b"", b"\xff" * (BLOB_ORG - end) + blob),
]
