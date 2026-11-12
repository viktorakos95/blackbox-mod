"""The crash recorder (src/crash.c): the fault vectors, the stock allocator's out-of-memory halt and the watchdog's check and stamps go to it. Apply with `cave`."""
import os
import struct

from thumb import ROOT, bl, symbols

sym = symbols(os.path.join(ROOT, "out", "cave.elf"))
STOCK = open(os.path.join(ROOT, "firmware", "BLACKBOX-3.1.9.bin"), "rb").read()


def movw_r2(imm):
    """MOVW r2, #imm (T3)."""
    hw1 = 0xF240 | ((imm >> 11) & 1) << 10 | (imm >> 12) & 15
    hw2 = ((imm >> 8) & 7) << 12 | 2 << 8 | imm & 0xFF
    return struct.pack("<HH", hw1, hw2)


assert movw_r2(0x1388) == bytes.fromhex("41f28832")


def vector(n):
    at = 0x08040000 + 4 * n
    return (at, STOCK[4 * n:4 * n + 4], struct.pack("<I", sym["crash_fault"] | 1))


PATCHES = [vector(3), vector(4), vector(5), vector(6),
           (0x0804003C, STOCK[0x3C:0x40], struct.pack("<I", sym["crash_systick_thunk"] | 1))] + [   # SysTick: who has the CPU
   # hard, memory, bus, usage fault (stock: spin until the watchdog)
    (0x08044452, bl(0x08044452, 0x08043DE4), bl(0x08044452, sym["crash_oom_thunk"])),   # the allocator's message before it halts
    (0x0804CB36, bl(0x0804CB36, 0x08046994), bl(0x0804CB36, sym["crash_wd"])),           # the audio block's watchdog check
    (0x080A2264, bl(0x080A2264, 0x080469CA), bl(0x080A2264, sym["crash_stamp_b"])),      # the GUI's stamp
    (0x080CFC24, struct.pack("<I", 0x08045975), struct.pack("<I", sym["crash_sd_read"] | 1)),   # the SD driver table: read (writes stay stock)
    (0x08045998, bytes.fromhex("41f28832"), movw_r2(1000)),                                        # the read's wait: 5000 -> 1000 ticks
] + [(a, bl(a, 0x080469C4), bl(a, sym["crash_stamp_a"]))                                   # the other loop's stamps
     for a in (0x08072BDE, 0x08072C96, 0x080740F4, 0x080743F2, 0x08074402, 0x080744CC, 0x08091DFE, 0x08092B6A)]
