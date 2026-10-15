"""SD recon image (milestone M7) under Unicorn: patches/mclspy.py and src/mcl_bb/bb_sdspy.cpp, and the numbers on the MCL page (src/mcl_bb/bb_ui.cpp, SD_SPY).

Checked: every stock call site of f_open / f_read / f_write / f_close / f_lseek / f_sync / f_mount now goes to a wrapper (and nothing else changed there), the card driver's
read / write / ioctl slots point at wrappers, the wrappers pass arguments and results through unchanged, calls are counted per task (default / audio / pcm streamer / other),
an entry from one task while another is inside the same layer counts as an overlap (nested calls of one task do not), the worst call time, the screen text.
NOT checked (hardware only): that pxCurrentTCB and the thread-handle globals hold what the wrappers expect (taken from the stock code, not measured), real overlaps.
"""
import os
import shutil
import struct
import subprocess
import sys

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs

from mcl_rig import Rig, emulate_primask, syms

NAMES = "solo slice duck chord cond filter cpu od comp seqfix fx2 munchi looper clkmon cave mclblob mclspy".split()
env = dict(os.environ, SDSPY="1", COMPACT="1", M3TEST="", M4TEST="", M5TEST="")
subprocess.run(["tools/mcl/build_blob.sh"], check=True, env=env, capture_output=True)
subprocess.run(["python3", "patch.py"] + NAMES, check=True, capture_output=True)
os.makedirs("out/mclspy-test", exist_ok=True)
shutil.copy("out/" + "+".join(NAMES) + "/BLACKBOX.bin", "out/mclspy-test/BLACKBOX.bin")
shutil.copy("out/mcl.elf", "out/mclspy-test/mcl.elf")
fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


BASE = 0x08040000
img = open("out/mclspy-test/BLACKBOX.bin", "rb").read()
stock = open("firmware/BLACKBOX-3.1.9.bin", "rb").read()
b = syms("out/mclspy-test/mcl.elf")
FAT = {0x08086534: "spy_f_open", 0x08086866: "spy_f_read", 0x08086a06: "spy_f_write", 0x08086d76: "spy_f_close", 0x08086d96: "spy_f_lseek", 0x08086c28: "spy_f_sync", 0x080864e8: "spy_f_mount"}

# --- the image: every call site of the FatFs functions is redirected, nothing else in the stock code changed
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
md.skipdata = True
sites = {}
for i in md.disasm(stock[:0xB2000], BASE):
    if i.mnemonic in ("bl", "b.w") and i.op_str.startswith("#") and int(i.op_str[1:], 16) in FAT:
        sites[i.address] = (i.mnemonic, FAT[int(i.op_str[1:], 16)])


def target(at):
    h1, h2 = struct.unpack("<HH", img[at - BASE:at - BASE + 4])
    s, j1, j2 = (h1 >> 10) & 1, (h2 >> 13) & 1, (h2 >> 11) & 1
    i1, i2 = 1 - (j1 ^ s), 1 - (j2 ^ s)
    off = (s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1)
    if s:
        off -= 1 << 25
    return at + 4 + off


ok = all(target(a) == (b[n] & ~1) for a, (m, n) in sites.items())
check("%d FatFs call sites (bl and tail b.w) all go to their wrappers" % len(sites), ok and len(sites) >= 85, len(sites))
names = {n for _, n in sites.values()}
check("all seven FatFs functions have call sites", names == set(FAT.values()), names - set(FAT.values()))
changed = [a for a in range(0, 0xB2000, 2) if img[a:a + 2] != stock[a:a + 2]]
allowed = set()
for a in sites:
    allowed |= {a - BASE, a - BASE + 2}
allowed |= {0x80cfc24 - BASE + k for k in range(12)} | {0x80f1e08 - BASE + k for k in range(4)} | {0x08044DFC - BASE + k for k in range(4)} | {0x080446D4 - BASE + k for k in range(4)}
allowed |= {0x0804015C - BASE + k for k in range(4)} | {0x08040118 - BASE + k for k in range(4)}
# clkmon's own three hooks and the other mod hooks change code too: only look at the FatFs and driver ranges
fat_range = [a for a in changed if 0x08086000 - BASE <= a < 0x08088000 - BASE or 0x08045900 - BASE <= a < 0x08045b00 - BASE]
check("the FatFs and card-driver code itself is untouched (only call sites elsewhere change)", not [a for a in fat_range if a not in allowed], [hex(a + BASE) for a in fat_range if a not in allowed][:5])
slots = struct.unpack("<3I", img[0x80cfc24 - BASE:0x80cfc24 - BASE + 12])
check("driver table: read / write / ioctl slots point at the wrappers, init / status untouched", slots == (b["spy_disk_read"] | 1, b["spy_disk_write"] | 1, b["spy_disk_ioctl"] | 1) and img[0x80cfc1c - BASE:0x80cfc24 - BASE] == stock[0x80cfc1c - BASE:0x80cfc24 - BASE])

# --- the wrappers
rg = Rig("out/mclspy-test")
e = rg.e
TCB = 0x2401f2a8
H = {"D": 0x24002e78, "A": 0x24002e74, "P": 0x24002e70}
tcbs = {"D": 0x24030000, "A": 0x24030400, "P": 0x24030800, "O": 0x24030c00}
for k, a in H.items():
    e.w32(a, tcbs[k])
e.uc.mem_map(0xE0000000, 0x100000) if False else None
orig = {}
dwt = {"v": 1000}


def make_stub(addr, ret, advance=0):
    def f():
        orig.setdefault(addr, []).append(tuple(e.arg(i) for i in range(4)))
        dwt["v"] += advance
        e.uc.mem_write(0xE0001004, struct.pack("<I", dwt["v"] & 0xFFFFFFFF))
        e.ret(ret)
    e.stub(addr, f)


for a, r in ((0x08086534, 0), (0x08086866, 0), (0x08086a06, 0), (0x08086d76, 0), (0x08086d96, 0), (0x08086c28, 0), (0x080864e8, 0), (0x08045974, 0), (0x080459e8, 0), (0x08045a5c, 0)):
    make_stub(a, r)
make_stub(0x08086866, 7)
e.w32(0xE000E014, 399_000 - 1)
rg.call("mcl_boot")
emulate_primask(rg)


def as_task(k):
    e.w32(TCB, tcbs[k])


def call(name, *a):
    return rg.call(name, *a)


for k, i in (("D", 0), ("A", 1), ("P", 2), ("O", 3)):
    as_task(k)
    check("task %s is recognised as class %d" % (k, i), call("bb_sdspy_task") == i, call("bb_sdspy_task"))
as_task("P")
orig.clear()
r = call("spy_f_read", 0x1111, 0x2222, 0x3333, 0x4444)
check("f_read: arguments and result pass through (4 args, result 7)", r == 7 and orig[0x08086866] == [(0x1111, 0x2222, 0x3333, 0x4444)], (r, orig))
as_task("D")
call("spy_f_open", 1, 2, 3, 0)
call("spy_f_open", 1, 2, 3, 0)
call("spy_f_write", 1, 2, 3, 0)
as_task("A")
call("spy_f_close", 1, 0, 0, 0)
as_task("O")
call("spy_f_lseek", 1, 0, 0, 0)
check("counts per task and function", (call("bb_sdspy_count", 2, 1), call("bb_sdspy_count", 0, 0), call("bb_sdspy_count", 0, 2), call("bb_sdspy_count", 1, 3), call("bb_sdspy_count", 3, 4)) == (1, 2, 1, 1, 1))
check("no overlaps from sequential calls", call("bb_sdspy_overlaps", 0) == 0 and call("bb_sdspy_overlaps", 1) == 0 and call("bb_sdspy_pair", 0) == 0xff)
as_task("P")
call("spy_disk_read", 0, 0x30000000, 5, 1)
call("spy_disk_write", 0, 0x30000000, 5, 1)
call("spy_disk_ioctl", 0, 1, 0, 0)
check("card driver calls counted and passed on", call("bb_sdspy_disk", 2, 0) == 1 and call("bb_sdspy_disk", 2, 1) == 1 and call("bb_sdspy_disk", 2, 2) == 1 and orig[0x08045974] == [(0, 0x30000000, 5, 1)])

# --- overlap: task P is inside the card driver, task D enters
call("bb_sdspy_enter", 1, 2)
call("bb_sdspy_enter", 1, 2)                                     # the same task nested: not an overlap
check("nested entry of the same task is not an overlap", call("bb_sdspy_overlaps", 1) == 0)
call("bb_sdspy_enter", 1, 0)
check("an entry while another task is inside counts as an overlap, first pair recorded (D entering, P inside)", call("bb_sdspy_overlaps", 1) == 1 and call("bb_sdspy_pair", 1) == 0x02, (call("bb_sdspy_overlaps", 1), hex(call("bb_sdspy_pair", 1))))
check("... in its own layer only", call("bb_sdspy_overlaps", 0) == 0)
call("bb_sdspy_leave", 1, 0)
call("bb_sdspy_leave", 1, 2)
call("bb_sdspy_leave", 1, 2)
call("bb_sdspy_enter", 1, 1)
check("after everything has left, a new entry is clean", call("bb_sdspy_overlaps", 1) == 1)
call("bb_sdspy_leave", 1, 1)
call("bb_sdspy_leave", 1, 1)                                     # a leave without an enter does not underflow
call("bb_sdspy_enter", 1, 0)
check("... also after a stray leave", call("bb_sdspy_overlaps", 1) == 1)
call("bb_sdspy_leave", 1, 0)

# --- worst call time (the stub advances the cycle counter by 3 ms at 399 MHz = 1,197,000 cycles)
make_stub(0x08086a06, 0, advance=1_197_000)
as_task("D")
call("spy_f_write", 0, 0, 0, 0)
check("worst call time measured: about 3000 us", 2900 <= call("bb_sdspy_worst_us", 0) <= 3100, call("bb_sdspy_worst_us", 0))

# --- the screen
def screen():
    p = rg.call("bb_ui_sig")
    fb = bytes(e.uc.mem_read(rg.call("bb_ui_fb"), 512))
    src = open("src/mcl_bb/glcdfont.h").read()
    font = [int(t, 16) for t in src[src.index("{") + 1:src.rindex("}")].replace("//", " ").replace(",", " ").split() if t.startswith("0x")]
    rows = []
    for row in range(4):
        s = ""
        for c in range(21):
            cols = [sum((fb[c * 6 + i + 128 * ((row * 8 + j) >> 3)] >> ((row * 8 + j) & 7) & 1) << j for j in range(7)) for i in range(5)]
            s += next((chr(k) for k in range(32, 127) if [font[k * 5 + i] & 0x7f for i in range(5)] == cols), "?")
        rows.append(s.rstrip())
    return rows


rows = screen()
check("view 0: header and a row per task (D A P)", rows[0].split() == ["OPN", "RD", "WR", "CL", "SK"] and [r[0] for r in rows[1:]] == ["D", "A", "P"], rows)
check("view 0: D opened twice, wrote twice; A closed once; P read once", rows[1].split() == ["D", "2", "0", "2", "0", "0"] and rows[2].split() == ["A", "0", "0", "0", "1", "0"] and rows[3].split() == ["P", "0", "1", "0", "0", "0"], rows)
rg.call("bb_ui_input", 2, 3, 0)
rg.call("bb_ui_input", 3, 3, 0)
rows = screen()
check("B4 switches to view 1: overlaps, worst times, card calls", rows[0].startswith("OVL F  0 D  1") and rows[1].startswith("MS  F  3") and rows[2].startswith("DRD") and rows[3].startswith("DWR"), rows)
check("view 1: the first overlap pair is shown (DP)", "DP" in rows[0].replace(" ", ""), rows[0])
check("view 1: card reads by P counted", "P   1" in rows[2] or "P  1" in rows[2], rows[2])
rg.call("bb_ui_input", 2, 3, 0)
check("B4 again: back to view 0", screen()[0].split()[0] == "OPN")

print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
