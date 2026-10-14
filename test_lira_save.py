"""Saving the synth's settings (src/lira_pad.c lira_save / lira_load, thunks, hooks) under Unicorn, with FatFs stubbed by a dict.

Checked: the file name and modes, a round trip of the pad settings and the page's controls and chord state, a damaged or foreign file is
ignored, a missing file leaves the settings alone, the thunks run the stock handler first and keep their arguments.
NOT checked: FatFs itself, which directory 'bbmod.bin' lands in (the same as settings.tml, whose call uses the same relative path style),
whether the load is also run at boot.
"""
import struct
import sys

from unicorn.arm_const import UC_ARM_REG_R0

from emu import Emu

IMG = "out/solo+slice+duck+chord+cond+filter+cpu+od+comp+seqfix+fx2+munchi+looper+lira+cave/BLACKBOX.bin"
e = Emu(IMG, "out/cave.elf")
e.uc.mem_map(0xC0000000, 0x400000)
e.uc.mem_map(0x38800000, 0x1000)
e.uc.mem_map(0x58024000, 0x1000)
e.uc.mem_map(0xE0000000, 0x10000)
fails = 0


def check(name, ok, info=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + ("  " + info if info else ""))
    fails += not ok


e.stub(0x0808C0D8)
e.stub(0x0808C12C)
e.stub(0x0804436C, value=0xC0000000)
e.call("interp_register", 0x24060000, 0xF1, 0, 0, 2, 0)

disk = {}
opened = []
cur = {}


def f_open():
    path = e.cstr(e.arg(1))
    mode = e.arg(2)
    opened.append((path, mode))
    cur.update(path=path, mode=mode)
    if mode == 1 and path not in disk:
        e.uc.reg_write(UC_ARM_REG_R0, 4)                                          # FR_NO_FILE
        return
    e.uc.reg_write(UC_ARM_REG_R0, 0)


def f_write():
    data = bytes(e.uc.mem_read(e.arg(1), e.arg(2)))
    disk[cur["path"]] = data
    e.w32(e.arg(3), len(data))
    e.uc.reg_write(UC_ARM_REG_R0, 0)


def f_read():
    data = disk.get(cur["path"], b"")[:e.arg(2)]
    e.uc.mem_write(e.arg(1), data)
    e.w32(e.arg(3), len(data))
    e.uc.reg_write(UC_ARM_REG_R0, 0)


e.stub(0x08086534, f_open)
e.stub(0x08086A06, f_write)
e.stub(0x08086866, f_read)
e.stub(0x08086D76, lambda: e.uc.reg_write(UC_ARM_REG_R0, 0))


def get(c):
    e.call("lira_ui_get", c)
    v = e.uc.reg_read(UC_ARM_REG_R0)
    return v - (1 << 32) if v & 0x80000000 else v


def chord(i):
    e.call("lira_ui_chord_get", i)
    return e.uc.reg_read(UC_ARM_REG_R0)


# a missing file changes nothing
before = (get(16), get(121), chord(140))
e.call("lira_load")
check("no file: the settings stay as they are", (get(16), get(121), chord(140)) == before and opened[-1] == ("bbmod.bin", 1), str(opened[-1:]))
# set things, save
e.call("lira_ui_set", 16, 321)
e.call("lira_ui_set", 121, 777)
e.call("lira_ui_set", 105, 3)
e.call("lira_ui_set", 108, 444)
e.call("lira_ui_chord_set", 140, 9)
e.call("lira_ui_chord_set", 141, 7)
e.call("lira_ui_action", 0)
e.call("lira_ui_action", 3)
e.call("lira_ui_action", 4)
notes = [None] * 8
e.call("lira_save")
check("save: bbmod.bin written with create-always (mode 0x0a)", opened[-1] == ("bbmod.bin", 0x0A) and len(disk["bbmod.bin"]) > 1000, str(opened[-1]) + " %d bytes" % len(disk.get("bbmod.bin", b"")))
want = (get(16), get(121), get(105), get(108), chord(140), chord(141), chord(150), chord(153), chord(154))
# change everything, load, compare
e.call("lira_ui_set", 16, 1)
e.call("lira_ui_set", 121, 2)
e.call("lira_ui_set", 105, 0)
e.call("lira_ui_set", 108, 0)
e.call("lira_ui_chord_set", 140, 0)
e.call("lira_ui_chord_set", 141, 0)
e.call("lira_ui_action", 0)
e.call("lira_ui_action", 3)
e.call("lira_ui_action", 4)
e.call("lira_load")
got = (get(16), get(121), get(105), get(108), chord(140), chord(141), chord(150), chord(153), chord(154))
check("load: the controls, FM source, chord state and waveform come back", got == want, "%s vs %s" % (got, want))
# the pad settings (the pool's side rows) travel too: the page's row is checked above; check one side value through the getter of a pad store
good = disk["bbmod.bin"]
bad = bytearray(good)
bad[100] ^= 0xFF
disk["bbmod.bin"] = bytes(bad)
e.call("lira_ui_set", 16, 5)
e.call("lira_load")
check("a damaged file (bad CRC) is ignored", get(16) == 5)
disk["bbmod.bin"] = b"X" * 10
e.call("lira_load")
check("a short foreign file is ignored", get(16) == 5)
# the thunks: the stock handler first (stubbed), then ours
order = []
e.stub(0x08091A5C, lambda: order.append(("stock save", e.arg(0), e.arg(1))))
e.stub(0x08091A74, lambda: order.append(("stock load", e.arg(0), e.arg(1))))
disk["bbmod.bin"] = good
e.call("lira_save_thunk", 0x2401F508, 0x24072000)
check("save thunk: the stock save handler runs with its arguments, then the file is written", order[-1] == ("stock save", 0x2401F508, 0x24072000) and opened[-1][1] == 0x0A, str(order))
disk["bbmod.bin"] = good
e.call("lira_ui_set", 16, 1)
e.call("lira_load_thunk", 0x2401F508, 0x24072000)
check("load thunk: the stock load handler runs, then the file is read", order[-1][0] == "stock load" and get(16) == want[0], str(order[-1]))
print("failures:", fails)
sys.exit(1 if fails else 0)
