"""findref.py <pool_addr> ...  — Thumb `ldr rX,[pc,#imm]` instructions that load the given literal-pool word."""
import sys
BASE = 0x08040000
fw = open(__file__.rsplit("/", 1)[0] + "/firmware/BLACKBOX-3.1.9.bin", "rb").read()
for t in sys.argv[1:]:
    t = int(t, 16)
    for a in range(max(BASE, t - 4096), t, 2):
        o = a - BASE
        h = int.from_bytes(fw[o:o + 2], "little")
        if h >> 11 == 0b01001 and ((a + 4) & ~3) + (h & 0xFF) * 4 == t:      # T1 ldr rt,[pc,#imm8*4]
            print(f"{t:#x} <- ldr r{(h >> 8) & 7} @ {a:#x}")
        h2 = int.from_bytes(fw[o + 2:o + 4], "little")
        if h & 0xFF7F == 0xF85F and ((a + 4) & ~3) + (1 if h & 0x80 else -1) * (h2 & 0xFFF) == t:  # T2 ldr.w
            print(f"{t:#x} <- ldr.w r{h2 >> 12} @ {a:#x}")
