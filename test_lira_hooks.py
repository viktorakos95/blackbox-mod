"""The save request is served from every place the stock streamer task passes: the loop's top (0x080743e8), the end of its inner work loop (0x0807443e: the pass that keeps running
while samples stream) and its idle pass (0x080744d8), with and without the MCL blob's own hooks at the first two (patches/lirasave.py, patches/mclsd.py).
Checked on the built images (the bytes at the three call sites, the cave's two 'next' pointers) and by calling the cave's hooks with the stock functions stubbed.
NOT checked (hardware only): that the task really reaches these places while voices play (it is what MCL's card access relies on too)."""
import os
import struct
import subprocess
import sys

from unicorn.arm_const import UC_ARM_REG_R0

from emu import Emu
sys.path.insert(0, "patches")
from thumb import bl, symbols

fails = 0


def check(name, ok, info=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + ("  " + str(info) if info else ""))
    fails += not ok


BASE = 0x08040000
csym = symbols("out/cave.elf")
SOLO = "solo slice duck chord cond filter cpu od comp tools seqfix fx2 munchi looper clkmon lira lirarec lirasave crash cave".split()


def build(extra):
    names = SOLO + extra
    subprocess.run(["python3", "patch.py"] + names, check=True, capture_output=True)
    return open("out/" + "+".join(names) + "/BLACKBOX.bin", "rb").read()


def word(img, a):
    return struct.unpack("<I", img[a - BASE:a - BASE + 4])[0]


img = build([])
for site, stock, fn in ((0x080743E8, 0x08088756, "lira_sd_top"), (0x0807443E, 0x08092F5C, "lira_sd_inner"), (0x080744D8, 0x08092F5C, "lira_file_poll")):
    check(f"without MCL: the call at {site:#x} goes to the cave's {fn}", img[site - BASE:site - BASE + 4] == bl(site, csym[fn]))
check("... and the cave's 'next' pointers are the stock functions", word(img, csym["lira_sd_next_top"]) == 0x08088757 and word(img, csym["lira_sd_next_inner"]) == 0x08092F5D)
if os.path.exists("out/mcl.elf"):
    bsym = symbols("out/mcl.elf")
    img2 = build(["mclblob", "mclsd"])
    check("with the MCL blob and mclsd: the same three calls stay the cave's", all(img2[a - BASE:a - BASE + 4] == bl(a, csym[f]) for a, f in ((0x080743E8, "lira_sd_top"), (0x0807443E, "lira_sd_inner"), (0x080744D8, "lira_file_poll"))))
    check("... and the 'next' pointers are MCL's hooks (mcl_sd_pump, mcl_sd_inner)", word(img2, csym["lira_sd_next_top"]) == (bsym["mcl_sd_pump"] | 1) and word(img2, csym["lira_sd_next_inner"]) == (bsym["mcl_sd_inner"] | 1), (hex(word(img2, csym["lira_sd_next_top"])), hex(bsym["mcl_sd_pump"])))
img3 = build(["mclblob", "mclsd"])

# the hooks, called with the stock functions stubbed
IMG = "out/solo+slice+duck+chord+cond+filter+cpu+od+comp+seqfix+fx2+munchi+looper+lira+cave/BLACKBOX.bin"
subprocess.run(["python3", "patch.py", "solo", "slice", "duck", "chord", "cond", "filter", "cpu", "od", "comp", "seqfix", "fx2", "munchi", "looper", "lira", "cave"], check=True, capture_output=True)
e = Emu(IMG, "out/cave.elf")
e.uc.mem_map(0xC0000000, 0x400000)
e.uc.mem_map(0x38800000, 0x1000)
e.uc.mem_map(0x58024000, 0x1000)
e.uc.mem_map(0xE0000000, 0x10000)
e.stub(0x0808C0D8)
e.stub(0x0808C12C)
e.stub(0x0804436C, value=0xC0000000)
e.w32(0x24000088, 0xC4000000)
e.w32(0x2400008C, 0xC0000000)
e.call("interp_register", 0x24060000, 0xF1, 0, 0, 2, 0)
opened = []
files, cur = {}, {}
def f_open():
    path = e.cstr(e.arg(1))
    opened.append(path)
    cur.update(path=path, mode=e.arg(2), pos=0)
    if e.arg(2) & 8 or e.arg(2) == 0x0a:
        files[path] = b""
    e.uc.reg_write(UC_ARM_REG_R0, 0 if path in files or e.arg(2) != 1 else 4)
def f_write():
    data = bytes(e.uc.mem_read(e.arg(1), e.arg(2)))
    files[cur["path"]] = files.get(cur["path"], b"") + data
    e.w32(e.arg(3), len(data))
    e.uc.reg_write(UC_ARM_REG_R0, 0)
def f_read():
    d = files.get(cur["path"], b"")[cur["pos"]:cur["pos"] + e.arg(2)]
    e.uc.mem_write(e.arg(1), d)
    cur["pos"] += len(d)
    e.w32(e.arg(3), len(d))
    e.uc.reg_write(UC_ARM_REG_R0, 0)
e.stub(0x08086534, f_open)
e.stub(0x08086A06, f_write)
e.stub(0x08086D76, lambda: e.uc.reg_write(UC_ARM_REG_R0, 0))
e.stub(0x08086866, f_read)
stock = []
e.stub(0x08088756, lambda: (stock.append("top"), e.uc.reg_write(UC_ARM_REG_R0, 1)))
e.stub(0x08092F5C, lambda: stock.append("inner"))
def state():
    e.call("lira_ui_save", 1)
    return e.uc.reg_read(UC_ARM_REG_R0)
for fn, args, tag in (("lira_sd_top", (0x24001000, 0x24001100, 0, 0), "top"), ("lira_sd_inner", (0x2401F508,), "inner"), ("lira_file_poll", (0x2401F508,), "inner")):
    opened.clear()
    stock.clear()
    e.call("lira_ui_save", 0)
    e.call(fn, *args)
    check(f"{fn}: a save asked is done on that pass (files opened, SAVED), and the stock function still runs once", state() == 2 and "bbmod.bin" in opened and "bbgen.bin" in opened and stock == [tag], (state(), opened[:2], stock))
    for _ in range(1):
        e.call("lira_ui_save", 0)                                   # (reset to idle for the next: wait out the SAVED state by asking again)
e.call("lira_sd_top", 0x24001000, 0x24001100, 0, 0)
check("lira_sd_top returns the stock queue call's result", e.uc.reg_read(UC_ARM_REG_R0) == 1)
print("failures:", fails)
sys.exit(1 if fails else 0)
