"""SD card for MCL (milestone M7). Apply AFTER `mclblob` (the hook is in the blob):  python3 patch.py ... cave mclblob mclsd

One call in the stock pcmStreamer loop is redirected: 0x080743e8 `bl 0x08088756` (the loop's non-blocking take from its command queue, run again and again whenever
the task gets the CPU) -> mcl_sd_pump, which first runs a file request posted by MCL (one FatFs call) and then makes the same queue call. The stock streamer is the only task
that ever touches the card (measured, docs/mcl-m0-m1-results.md), so MCL's file work runs there too. No-op until the blob has booted and set its ready flag."""
import os
import struct
import sys

from thumb import ROOT, bl, symbols

bsym = symbols(os.path.join(ROOT, "out", "mcl.elf"))
if "lirasave" in sys.argv[1:]:
    # the cave's hooks (patches/lirasave.py) already sit at both places and serve the save requests; they call these two functions afterwards: the cave's own variables are pointed at the blob
    from thumb import symbols as _symbols
    csym = _symbols(os.path.join(ROOT, "out", "cave.elf"))
    PATCHES = [
        (csym["lira_sd_next_top"], struct.pack("<I", 0x08088757), struct.pack("<I", bsym["mcl_sd_pump"] | 1)),
        (csym["lira_sd_next_inner"], struct.pack("<I", 0x08092F5D), struct.pack("<I", bsym["mcl_sd_inner"] | 1)),
    ]
else:
    PATCHES = [
        (0x080743E8, bl(0x080743E8, 0x08088756), bl(0x080743E8, bsym["mcl_sd_pump"])),
        # ... and once a pass of its inner work loop, which runs without a break while sequences stream (the top above is not reached then)
        (0x0807443E, bl(0x0807443E, 0x08092F5C), bl(0x0807443E, bsym["mcl_sd_inner"])),
    ]

# (n29: the per-f_read hook (mcl_sd_fread, 30 call sites in pcmStreamer) is out again. It was added for folders failing while a sequence played; the cause was MCL's shared
# current folder (fixed in the browser), and the unit restarted by itself with MCL open. The function stays in the blob, unused.)
