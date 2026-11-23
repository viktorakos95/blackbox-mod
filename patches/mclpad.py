"""Flash window test for the MCL port (milestone M2 gate). Apply AFTER `cave`:  python3 patch.py ... cave mclpad

Pads the image with 0xFF up to 0x08140000, then appends 256 KB of a known word pattern at 0x08140000..0x08180000, so the image ends at
0x08180000 (1,310,720 bytes). That is two flash sectors past the largest window tested so far (0x08140000). The firmware (src/cpu.c, pad_step)
reads the pattern back 2048 words a second and shows " P37%" / " POK" / " Fxxxx" on the version label. Nothing past 0x08180000 is touched.
"""
import os
import zlib

from thumb import ROOT, symbols

BASE, STOCK_END = 0x08040000, 0x080F1E78
PAD_BASE = int(os.environ.get("MCLPAD_BASE", "0x08140000"), 16)
PAD_END = int(os.environ.get("MCLPAD_END", "0x08180000"), 16)          # MCLPAD_END=0x081E0000: the next window (0x081E0000 and up stay untouched)
assert PAD_END <= 0x081E0000 and PAD_BASE < PAD_END
itcm = os.path.exists(os.path.join(ROOT, "out", "itcm.bin"))           # cave.py has filled 0xFF up to the ITCM image at 0x081D0000: the pad goes over that fill
assert not itcm or PAD_END <= 0x081D0000, "the pad would overwrite the ITCM image"
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
build_id = (zlib.crc32(cave) ^ 0x4D504144 ^ PAD_END) & M or 1

PATCHES = [
    (sym["mclpad_on"], bytes(4), build_id.to_bytes(4, "little")),
    (sym["mclpad_base"], (0x08140000).to_bytes(4, "little"), PAD_BASE.to_bytes(4, "little")),
    (sym["mclpad_words"], (65536).to_bytes(4, "little"), ((PAD_END - PAD_BASE) // 4).to_bytes(4, "little")),
    (PAD_BASE, b"\xff" * len(pad), pad) if itcm else (end, b"", gap + pad),
]
