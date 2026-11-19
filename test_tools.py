"""Our general settings on the Tools pages (patches/tools.py, src/comp.c tools_get): the page lists in the image, ids clear of the stock
and the mod's other ones, and tools_get through the real settings lookup. NOT checked: how the stock Tools screen draws the longer lists,
and the settings.tml round trip (unit)."""
import re
import struct
import sys

from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB

from emu import Emu

IMG = "out/solo+slice+duck+chord+cond+filter+cpu+od+comp+tools+seqfix+fx2+munchi+cave/BLACKBOX.bin"
e = Emu(IMG, "out/cave.elf")
STORE, ARR = 0x24020088 + 0x8C90, 0x24070000
fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


def page(a):
    out = []
    for k in range(36):
        v = e.r16(a + 2 * k)
        if not v:
            break
        out.append(v)
    return out


p0, p3, p2 = page(0x080EED18), page(0x080EEDF0), page(0x080EEDA8)
check("Tools Main: the comp order, untouched by us (11 ids)", p0 == [0xBA, 0xB4, 0x1D0, 0xBB, 0xBC, 0xBD, 0x7F, 0xCD, 0xF6, 0xF7, 0x19B], [hex(v) for v in p0])
check("Tools page 2: its six, then Mono Input, DRONE-8 Out, SAMPLR Out, Machine Filters", p2 == [0x76, 0xE2, 0x79, 0xD3, 0x71, 0x7B, 0x1A2, 0x1A3, 0x1A4, 0x1A5], [hex(v) for v in p2])
check("no Tools page has more than 11 ids (the stock builder's structure holds 11)", all(len(page(0x080EED18 + 0x48 * k)) <= 11 for k in range(4)))
check("clock page: Analog In / Out, then Clock Out 2 and Clock Level", p3 == [0xEF, 0xF0, 0x10F, 0x110, 0x1A0, 0x1A1], [hex(v) for v in p3])
check("the next page slots are untouched (page 1 and the one after the clock page)", page(0x080EED60) == [0x9D, 0xE1, 0xD5] and page(0x080EEE38) == [])

# ids 0x1a0..0x1a5 are not registered by the stock firmware
stock = open("firmware/BLACKBOX-3.1.9.bin", "rb").read()
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
md.skipdata = True
ids = set()
for x in md.disasm(stock[0x0808C000 - 0x08040000:0x08090000 - 0x08040000], 0x0808C000):
    m = re.match(r"r1, #(0x[0-9a-f]+|\d+)$", x.op_str)
    if x.mnemonic in ("mov", "movw", "movs", "mov.w") and m:
        ids.add(int(m.group(1), 0))
check("ids 0x1a0..0x1a5 are not registered by the stock firmware (stock goes to 0x19b)", not any(0x1A0 <= v <= 0x1A5 for v in ids), sorted(hex(v) for v in ids if v >= 0x190))
check("... and are inside the registration table (500 slots)", 0x1A5 < 500)

# tools_get through the real lookup (e.call returns the recorded calls: the result is r0)
from unicorn.arm_const import UC_ARM_REG_R0


def tg(i):
    e.call("tools_get", i)
    return e.uc.reg_read(UC_ARM_REG_R0)



e.uc.mem_write(ARR, struct.pack("<HHI", 0x1A0, 0, 4) + struct.pack("<HHI", 0x1A1, 0, 500) + struct.pack("<HHI", 0x1A2, 0, 1))
e.w32(STORE + 4, ARR)
e.uc.mem_write(STORE + 10, struct.pack("<H", 3))
check("tools_get reads the clock PPQ, level and mono settings", [tg(0x1A0), tg(0x1A1), tg(0x1A2)] == [4, 500, 1], [tg(i) for i in (0x1A0, 0x1A1, 0x1A2)])
check("an id that is not in the store reads 0 (the default)", tg(0x1A3) == 0)
print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
