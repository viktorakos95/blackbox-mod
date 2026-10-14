"""Flash window test image (patches/mclpad.py, src/cpu.c pad_step) under Unicorn.

Real code runs for: the pad readback and the label text (cpu_wait -> report -> pad_step / compose) on the padded image.
Checked: the image layout (size, gap, pattern), progress " P37%" -> " POK", a corrupted or erased word -> " Fxxxx" with its index,
the result survives, a different build id restarts the scan, and a normal build (flag 0) never reads the pad region.
NOT checked (hardware only): that the updater writes the 1.3 MB image and the unit reads the pattern back.
"""
import glob
import struct
import sys

from emu import Emu

IMG = glob.glob("out/*+cave+mclpad/BLACKBOX.bin")[0]
BASE, PAD_BASE, PAD_END = 0x08040000, 0x08140000, 0x08180000
image = open(IMG, "rb").read()
fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


def make(e=None):
    e = e or Emu(IMG, "out/cave.elf")
    e.uc.mem_map(0xE0000000, 0x100000)
    e.uc.mem_map(0x38800000, 0x1000)
    e.uc.mem_map(0x58024000, 0x1000)
    e.w32(0x24000088, 0xC4000000)
    e.w32(0x2400008C, 0xC4000000 - 41 * 1024 * 1024)
    e.stub(0x08088756, lambda: (e.w32(0xE0001004, e.r32(0xE0001004) + 2_560_000), e.ret(1)))
    e.stub(0x080A3DEC)
    e.uc.mem_write(0x24060000, bytes(0x60))
    e.uc.mem_write(0x24060000 + 0x31, e.cstr(0x080CF290).encode() + b"\0")
    e.uc.mem_write(0x2405FFD0, b"\xa5" * 36)
    e.call("cpu_text_b", 0x24060000, 0x080CF290)
    return e


def label(e):
    return e.cstr(0x24060000 + 0x31)


def second(e):
    for _ in range(188):
        e.call("cpu_wait", 0x24001F00, 0x24001F80, 2000, 0)


# --- layout
check("image is 1,310,720 bytes and ends at 0x08180000", len(image) == 0x140000 and BASE + len(image) == PAD_END, len(image))
cave_end = 0x080F1E78 + len(open("out/cave.bin", "rb").read())
check("0xFF gap from the cave end to 0x08140000", image[cave_end - BASE:PAD_BASE - BASE] == b"\xff" * (PAD_BASE - cave_end))
words = struct.unpack("<65536I", image[PAD_BASE - BASE:])
check("pad pattern has no repeats (65,536 distinct words)", len(set(words)) == 65536)
stock = open("firmware/BLACKBOX-3.1.9.bin", "rb").read()
diff = [i for i in range(len(stock)) if stock[i] != image[i]]
check("only the stock bytes the cave patches differ (below 0x080F1E78, not the pad)", max(diff) < 0xB1E78 and len(image) > len(stock))

# --- good pad: the result is on the label as soon as it is built at boot
e = make()
check("label built at boot of the padded image already reads ' POK'", " POK" in label(e), label(e))
check("label stays within the 30 bytes the text object allows", len(label(e)) <= 30, label(e))
check("result in backup SRAM: id set, no bad word", e.r32(0x38800910) == e.r32(e.sym["mclpad_on"]) and (e.r32(0x38800918) & 0xFFFFFFFF) == 0xFFFFFFFF)
for _ in range(3):
    second(e)
check("still ' POK' after the meter has run (no rescan, label unchanged)", " POK" in label(e), label(e))

# --- a flipped word, and an unwritten (erased) tail
for name, patch, idx in (("one flipped word", lambda u: u.mem_write(PAD_BASE + 4 * 5000, b"\x00\x00\x00\x00"), 5000),
                         ("erased tail (0xFF from 0x08170000)", lambda u: u.mem_write(0x08170000, b"\xff" * 0x10000), 0xC000),
                         ("first word wrong", lambda u: u.mem_write(PAD_BASE, b"\xef\xbe\xad\xde"), 0)):
    e = Emu(IMG, "out/cave.elf")
    patch(e.uc)
    e = make(e)
    check(f"{name}: ' F{idx:04x}'", f" F{idx:04x}" in label(e), label(e))

# --- power-up rescans; a different build id also rescans
e = make()
check("scan done", " POK" in label(e), label(e))
e.uc.mem_write(PAD_BASE + 4 * 77, b"\x00\x00\x00\x00")
e.call("cpu_text_b", 0x24060000, 0x080CF290)
check("same id, state kept: no rescan, still ' POK'", " POK" in label(e), label(e))
e.uc.mem_write(e.sym["mclpad_on"], struct.pack("<I", 0x12345678))
e.call("cpu_text_b", 0x24060000, 0x080CF290)
check("a different build id rescans and finds the damage (' F004d')", " F004d" in label(e), label(e))
e.uc.mem_write(e.sym["mclpad_on"], struct.pack("<I", e.r32(0x38800910) & 0xFFFFFFFF))
e.uc.mem_write(0x2405FFD0, b"\xa5" * 36)               # power-up: patch RAM garbage -> start() clears the state
e.call("cpu_text_b", 0x24060000, 0x080CF290)
check("power-up (start()) clears the result, the next label rescans", " F004d" in label(e), label(e))

# --- flag 0 (normal build): the pad region is never read
e = Emu(IMG, "out/cave.elf")
e.uc.mem_write(e.sym["mclpad_on"], bytes(4))
reads = []
from unicorn import UC_HOOK_MEM_READ
e.uc.hook_add(UC_HOOK_MEM_READ, lambda uc, t, a, sz, v, _: reads.append(a), begin=PAD_BASE, end=PAD_END - 1)
e = make(e)
for _ in range(3):
    second(e)
check("flag 0: no read from 0x08140000..0x08180000 and no pad text", not reads and " P" not in label(e) and " F" not in label(e), (len(reads), label(e)))

print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
