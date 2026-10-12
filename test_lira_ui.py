"""LIRA's pages and settings against the REAL stock page builder (FUN_08098d40) and store functions, under Unicorn.

Nothing about the builder or the stores is stubbed: a Sample pad's store is laid out as the firmware lays it out (APP +
0x1840 + row * 0xf0 + col * 0x30; vtable at +0, entries at +4, type byte at +8, count at +0xa, capacity at +0xc), filled
to the 59 entries of a stock pad (type 3), with the firmware's own vtable (0x080eea9c: slot 0 returns the type byte). The
stock page lists come from the firmware's own tables. The registrars and the allocator are stubbed (as in test_lira_pad.py).
"""
import struct
import sys

from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP

from emu import Emu

IMG = "out/solo+slice+duck+chord+cond+filter+cpu+od+comp+seqfix+fx2+munchi+lira+cave/BLACKBOX.bin"
e = Emu(IMG, "out/cave.elf")
e.uc.mem_map(0xC0000000, 0x200000)               # SDRAM heap
e.uc.mem_map(0x38800000, 0x1000)                 # backup SRAM
e.uc.mem_map(0x58024000, 0x1000)
e.uc.mem_map(0xE0000000, 0x10000)
fails = 0


def check(name, ok, info=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + ("  " + info if info else ""))
    fails += not ok


P0, NCTL = 0x1DB, 25
INTERP_LIRA = 14
APP = 0x24020088
ROW, COL = 1, 2
REC = APP + 0x1840 + ROW * 0xF0 + COL * 0x30
ARR, CELL, LIST = 0x24062000, 0x24062800, 0x24063000
VT_PAD = 0x080EEA9C
# the 59 ids a stock pad (type 3) is created with
STOCK_IDS = [0x11, 0x17c, 0x17e, 0x17f, 0x180, 0x189, 0x3c, 0x3e, 0x3f, 0x4, 0x40, 0x47, 0x4d, 0x4e, 0x5b, 0x5c, 0x5d, 0x62,
             0x6a, 0x6c, 0x8, 0x8b, 0x8d, 0x8e, 0x8f, 0x9, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x97, 0x98, 0x9a, 0xa, 0xa0,
             0xa1, 0xa2, 0xb, 0xc, 0xc4, 0xc8, 0xce, 0xcf, 0xd, 0xd0, 0xd2, 0xd9, 0xda, 0xdb, 0xdc, 0xea, 0xf1, 0xf2, 0xf3,
             0xf4, 0xf5, 0xfa]
assert len(STOCK_IDS) == 59

# registration at boot: the pool (with every pad's defaults) is taken from the heap
e.stub(0x0808C0D8)
e.stub(0x0808C12C)
e.stub(0x0804436C, value=0xC0000000)
e.call("interp_register", 0x24060000, 0xF1, 0, 0, 2, 0)


def make_pad(mode=0, fidelity=0, typ=3, loop=1):
    vals = {i: 0 for i in STOCK_IDS}
    vals.update({0x8B: mode, 0xF1: fidelity, 0x8E: loop})
    data = b"".join(struct.pack("<HHI", k, 0, v) for k, v in vals.items())
    e.uc.mem_write(ARR, data + b"\0" * (64 * 8 - len(data)))
    e.uc.mem_write(REC, struct.pack("<IIBBHH", VT_PAD, ARR, typ, 0, len(vals), 64))


def pages(p, cell_id=(ROW << 4) | COL, fn="lira_pages"):
    e.uc.mem_write(CELL, b"\0" * 64)
    e.uc.mem_write(CELL + 4, struct.pack("<H", cell_id))
    e.uc.mem_write(LIST, b"\0" * 0x400)
    e.call(fn, APP, CELL, LIST, p)
    cnt = e.r32(LIST)
    return [(e.r16(LIST + 12 * i + 4), e.r32(LIST + 12 * i + 12)) for i in range(cnt)]


def ids(p, **kw):
    return [i for i, _ in pages(p, **kw)]


def call_keep_r1(addr, *args):
    e.call(addr, *args)
    return e.uc.reg_read(UC_ARM_REG_R0)


# ---- the store getter / setter, entered through the firmware's own addresses ----
make_pad(fidelity=INTERP_LIRA)
GET, SET = 0x08093E9C, 0x08093ECC
check("getter (patched stock entry): a LIRA id on a pad store = its default (Tune 2 78/127)",
      call_keep_r1(GET, REC, P0 + 1) == round(78 * 1000 / 127))
check("getter: a stock id is read from the store (Loop Mode 1, Fidelity LIRA)",
      call_keep_r1(GET, REC, 0x8E) == 1 and call_keep_r1(GET, REC, 0xF1) == INTERP_LIRA)
check("getter: an id the store does not hold reads 0", call_keep_r1(GET, REC, 0x123) == 0)
e.uc.reg_write(UC_ARM_REG_R1, 0)
e.call(GET, REC, P0)
e.uc.reg_write(UC_ARM_REG_R0, 0)
n0 = e.r16(REC + 10)
e.call(SET, REC, P0 + 1, 500)
check("setter: a LIRA id goes to the pool, the store keeps its 59 entries",
      call_keep_r1(GET, REC, P0 + 1) == 500 and e.r16(REC + 10) == n0 == 59)
e.call(SET, REC, 0x8E, 0)
check("setter: a stock id changes in the store", call_keep_r1(GET, REC, 0x8E) == 0)
e.call(SET, REC, 0x777, 9)
check("setter: an id the store does not hold is ignored", e.r16(REC + 10) == 59 and call_keep_r1(GET, REC, 0x777) == 0)
other = REC + 0x30                                  # the pad next to it: its own values
check("each pad position has its own LIRA values", call_keep_r1(GET, other, P0 + 1) == round(78 * 1000 / 127))
e.call(SET, REC, P0 + 1, round(78 * 1000 / 127))

# ---- the pages, through the real builder ----
make_pad(mode=0, fidelity=0)
stock = {p: ids(p, fn=0x08098D40) for p in range(5)}
print("stock Sample pages:", {p: [hex(x) for x in v] for p, v in stock.items()})
check("stock builder on this pad record: page 2 = Loop Mode, Loop Fade, Reverse, Start, Length, Loop Start, Loop End",
      stock[1] == [0x8E, 0xFA, 0x6C, 0x8F, 0x90, 0x91, 0x92])
make_pad(mode=0, fidelity=INTERP_LIRA)
got = {p: ids(p) for p in range(5)}
print("LIRA pages:", {p: [hex(x) for x in v] for p, v in got.items()})
shown = [x for p in (0, 1, 2) for x in got[p] if x >= P0]
check("21 LIRA controls on pages 1..3 once each (Mod Src, Dly Mod, Vibrato stay at defaults)", sorted(shown) == [P0 + i for i in range(NCTL) if i not in (13, 20, 21)])
check("no LIRA page lists more than 8 entries", all(len(got[p]) <= 8 for p in (0, 1, 2)))
check("page 5 is the stock one", got[4] == stock[4])
check("Loop Mode is the first entry of page 2", got[1][0] == 0x8E)
check("page 1 keeps Level", got[0][0] == 4)
pg1 = dict(pages(1))
check("a page lists the pool's values (Tune 2)", pg1[P0 + 1] == round(78 * 1000 / 127), str(pg1))
e.call(SET, REC, P0 + 1, 321)
check("a change made through the setter shows in the page", dict(pages(1))[P0 + 1] == 321)
e.call(SET, REC, P0 + 1, round(78 * 1000 / 127))

make_pad(mode=0, fidelity=0)
check("Fidelity Normal: stock pages", all(ids(p) == stock[p] for p in range(5)))
make_pad(mode=2, fidelity=INTERP_LIRA)
check("Slicer pad: no LIRA entries", not any(x >= P0 for p in range(5) for x in ids(p)))
make_pad(mode=0, fidelity=INTERP_LIRA, typ=0x4C)
check("the reduced pad store (type 0x4c) works too", any(x >= P0 for x in ids(1)))
sys.exit(1 if fails else 0)
