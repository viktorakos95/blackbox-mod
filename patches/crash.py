"""The crash recorder (src/crash.c): the fault vectors, the stock allocator's out-of-memory halt and the watchdog's check and stamps go to it. Apply with `cave`."""
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
    (0x0804CB36, bl(0x0804CB36, 0x08046994), bl(0x0804CB36, sym["crash_wd"])),           # the audio block's watchdog check
    (0x080A2264, bl(0x080A2264, 0x080469CA), bl(0x080A2264, sym["crash_stamp_b"])),      # the GUI's stamp
] + [(a, bl(a, 0x080469C4), bl(a, sym["crash_stamp_a"]))                                   # the other loop's stamps
     for a in (0x08072BDE, 0x08072C96, 0x080740F4, 0x080743F2, 0x08074402, 0x080744CC, 0x08091DFE, 0x08092B6A)]
