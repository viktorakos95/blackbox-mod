"""Flash window test for the MCL port (milestone M2 gate). Apply AFTER `cave`:  python3 patch.py ... cave mclpad

Pads the image with 0xFF up to 0x08140000, then appends 256 KB of a known word pattern at 0x08140000..0x08180000, so the image ends at
0x08180000 (1,310,720 bytes). That is two flash sectors past the largest window tested so far (0x08140000). The firmware (src/cpu.c, pad_step)
reads the pattern back 2048 words a second and shows " P37%" / " POK" / " Fxxxx" on the version label. Nothing past 0x08180000 is touched.
"""
import os
import zlib

from thumb import ROOT, symbols

BASE, STOCK_END = 0x08040000, 0x080F1E78
PAD_BASE, PAD_END = 0x08140000, 0x08180000
M = 0xFFFFFFFF

cave = open(os.path.join(ROOT, "out", "cave.bin"), "rb").read()
sym = symbols(os.path.join(ROOT, "out", "cave.elf"))
end = STOCK_END + len(cave)
assert end <= PAD_BASE, f"cave ends at {end:#x}, past the pad start"


def pad_word(i):                      # same as pad_word() in src/cpu.c
    w = (i * 2654435761) & M
    w ^= w >> 15
    w = (w * 2246822519) & M
    return w ^ (w >> 13)


pad = b"".join(pad_word(i).to_bytes(4, "little") for i in range((PAD_END - PAD_BASE) // 4))
gap = b"\xff" * (PAD_BASE - end)
build_id = (zlib.crc32(cave) ^ 0x4D504144) & M or 1

PATCHES = [
    (sym["mclpad_on"], bytes(4), build_id.to_bytes(4, "little")),
    (end, b"", gap + pad),
]
