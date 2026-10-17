"""The crash recorder (src/crash.c): the fault vectors and the stock allocator's out-of-memory halt go to it. Apply with `cave`."""
import os
import struct

from thumb import ROOT, bl, symbols

sym = symbols(os.path.join(ROOT, "out", "cave.elf"))
STOCK = open(os.path.join(ROOT, "firmware", "BLACKBOX-3.1.9.bin"), "rb").read()


def vector(n):
    at = 0x08040000 + 4 * n
    return (at, STOCK[4 * n:4 * n + 4], struct.pack("<I", sym["crash_fault"] | 1))


PATCHES = [vector(3), vector(4), vector(5), vector(6)] + [   # hard, memory, bus, usage fault (stock: spin until the watchdog)
    (0x08044452, bl(0x08044452, 0x08043DE4), bl(0x08044452, sym["crash_oom_thunk"])),   # the allocator's message before it halts
]
