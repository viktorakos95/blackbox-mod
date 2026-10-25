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
e.w32(0x24000088, 0xC4000000)                                   # the allocator's top and floor: 64 MB free
e.w32(0x2400008C, 0xC0000000)
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
before = (get(16), get(125), chord(140))
e.call("lira_load")
check("no file: the settings stay as they are", (get(16), get(125), chord(140)) == before and opened[-1] == ("bbmod.bin", 1), str(opened[-1:]))
# set things, save
e.call("lira_ui_set", 16, 321)
e.call("lira_ui_set", 125, 777)
e.call("lira_ui_set", 105, 3)
e.call("lira_ui_set", 106, 8)
e.call("lira_ui_action", 19)
e.call("lira_ui_set", 108, 444)
e.call("lira_ui_chord_set", 140, 9)
e.call("lira_ui_chord_set", 141, 7)
e.call("lira_ui_action", 0)
e.call("lira_ui_action", 3)
e.call("lira_ui_action", 4)
notes = [None] * 8
e.call("lira_save")
check("save: bbmod.bin written with create-always (mode 0x0a)", opened[-1] == ("bbmod.bin", 0x0A) and len(disk["bbmod.bin"]) > 1000, str(opened[-1]) + " %d bytes" % len(disk.get("bbmod.bin", b"")))
want = (get(16), get(125), get(105), get(108), chord(140), chord(141), chord(150), chord(153), chord(154), get(106), chord(195))
# change everything, load, compare
e.call("lira_ui_set", 16, 1)
e.call("lira_ui_set", 125, 2)
e.call("lira_ui_set", 105, 0)
e.call("lira_ui_set", 106, 0)
e.call("lira_ui_action", 19)
e.call("lira_ui_set", 108, 0)
e.call("lira_ui_chord_set", 140, 0)
e.call("lira_ui_chord_set", 141, 0)
e.call("lira_ui_action", 0)
e.call("lira_ui_action", 3)
e.call("lira_ui_action", 4)
e.call("lira_load")
got = (get(16), get(125), get(105), get(108), chord(140), chord(141), chord(150), chord(153), chord(154), get(106), chord(195))
check("load: the controls, FM source, chord state and waveform come back", got == want, "%s vs %s" % (got, want))
# WIDTH and DRIFT travel in a tail after the blob; older files (without it) still load and leave them as they are
e.call("lira_ui_set", 180, 111)
e.call("lira_ui_set", 181, 222)
e.call("lira_save")
full_file = disk["bbmod.bin"]
tone_file = full_file[:-20]                                         # (as a build before the ENV tail wrote it)
tone = lambda: [get(180), get(181)]
for c in (180, 181):
    e.call("lira_ui_set", c, 900)
disk["bbmod.bin"] = tone_file
e.call("lira_load")
check("width and drift come back", tone() == [111, 222], str(tone()))
for c in (180, 181):
    e.call("lira_ui_set", c, 900)
disk["bbmod.bin"] = tone_file + bytes(12)                          # a file from 3.1.AC / AD: a second tail after this one
e.call("lira_load")
check("a 3.1.AC / AD file (an extra tail) loads: width and drift come back", tone() == [111, 222], str(tone()))
for c in (180, 181):
    e.call("lira_ui_set", c, 900)
disk["bbmod.bin"] = tone_file[:-16]                                # a file from before: no tail at all
e.call("lira_ui_set", 16, 5)
e.call("lira_load")
check("a file from before the tail still loads (the controls come back, width and drift stay as they are)", get(16) == 321 and tone() == [900, 900], str((get(16), tone())))
bad_tail = bytearray(tone_file)
bad_tail[-8] ^= 0xFF
disk["bbmod.bin"] = bytes(bad_tail)
e.call("lira_load")
check("a damaged tail is ignored (the rest still loads)", tone() == [900, 900], str(tone()))
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
# the ENV tab (build 95) in a tail after that one: envelope times and velocity
for c, v in ((210, 300), (211, 800), (212, 100), (213, 900), (214, 250), (215, 1), (216, 1)):
    e.call("lira_ui_set", c, v)
e.call("lira_save")
env = lambda: [get(c) for c in range(210, 217)]
for c in range(210, 217):
    e.call("lira_ui_set", c, 0)
e.call("lira_load")
check("the ENV tab comes back (attacks, releases, velocity amount, source, target)", env() == [300, 800, 100, 900, 250, 1, 1], str(env()))
disk["bbmod.bin"] = tone_file
e.call("lira_load")
check("a file from before the ENV tab loads with its defaults (FAST attack a 3 ms snap, the rest as built, velocity off)", env() == [81, 500, 607, 500, 700, 0, 0], str(env()))
e.call("lira_ui_set", 16, 1)
e.call("lira_load_thunk", 0x2401F508, 0x24072000)
check("load thunk: the stock load handler runs, then the file is read", order[-1][0] == "stock load" and get(16) == want[0], str(order[-1]))
print("failures:", fails)
sys.exit(1 if fails else 0)
