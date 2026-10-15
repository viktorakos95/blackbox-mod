"""SD card for MCL (milestone M7). Apply AFTER `mclblob` (the hook is in the blob):  python3 patch.py ... cave mclblob mclsd

One call in the stock pcmStreamer loop is redirected: 0x080743e8 `bl 0x08088756` (the loop's non-blocking take from its command queue, run again and again whenever
the task gets the CPU) -> mcl_sd_pump, which first runs a file request posted by MCL (one FatFs call) and then makes the same queue call. The stock streamer is the only task
that ever touches the card (measured, docs/mcl-m0-m1-results.md), so MCL's file work runs there too. No-op until the blob has booted and set its ready flag."""
import os

from thumb import ROOT, bl, symbols

bsym = symbols(os.path.join(ROOT, "out", "mcl.elf"))
PATCHES = [
    (0x080743E8, bl(0x080743E8, 0x08088756), bl(0x080743E8, bsym["mcl_sd_pump"])),
]
