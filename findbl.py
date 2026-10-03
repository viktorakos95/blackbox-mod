"""findbl.py <target> ...  — every Thumb-2 BL/BLX/B.W in the stock image whose destination is <target>."""
import sys

BASE = 0x08040000
fw = open(__file__.rsplit("/", 1)[0] + "/firmware/BLACKBOX-3.1.9.bin", "rb").read()


def decode_bl(addr):
    o = addr - BASE
    h1 = int.from_bytes(fw[o:o + 2], "little")
    h2 = int.from_bytes(fw[o + 2:o + 4], "little")
    if h1 >> 11 != 0b11110 or (h2 >> 14) != 0b11:
        return None
    s = (h1 >> 10) & 1
    j1, j2 = (h2 >> 13) & 1, (h2 >> 11) & 1
    i1, i2 = 1 - (j1 ^ s), 1 - (j2 ^ s)
    imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1)
    if s:
        imm -= 1 << 25
    return addr + 4 + imm, "blx" if not (h2 >> 12) & 1 else "bl"


if __name__ == "__main__":
    for t in sys.argv[1:]:
        t = int(t, 16)
        for a in range(BASE, BASE + len(fw) - 4, 2):
            r = decode_bl(a)
            if r and r[0] & ~1 == t:
                print(f"{t:#010x} <- {r[1]} @ {a:#010x}")
