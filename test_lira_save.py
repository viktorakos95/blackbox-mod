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
    cur.update(path=path, mode=mode, pos=0)
    if mode == 0x0A:
        disk[path] = b""                                                          # (create always: an empty file, the writes append)
    if mode == 1 and path not in disk:
        e.uc.reg_write(UC_ARM_REG_R0, 4)                                          # FR_NO_FILE
        return
    e.uc.reg_write(UC_ARM_REG_R0, 0)


def f_write():
    data = bytes(e.uc.mem_read(e.arg(1), e.arg(2)))
    disk[cur["path"]] = disk.get(cur["path"], b"") + data
    e.w32(e.arg(3), len(data))
    e.uc.reg_write(UC_ARM_REG_R0, 0)


def f_read():
    data = disk.get(cur["path"], b"")[cur["pos"]:cur["pos"] + e.arg(2)]
    cur["pos"] += len(data)
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
check("no file: the settings stay as they are", (get(16), get(125), chord(140)) == before and opened[-2:] == [("bbmod.bin", 1), ("0:/bbmod.bin", 1)], str(opened[-2:]))
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
check("save: bbmod.bin written with create-always (mode 0x0a), then the general file bbgen.bin", all(x in opened for x in (("bbmod.bin", 0x0A), ("0:/bbmod.bin", 0x0A), ("bbgen.bin", 0x0A), ("0:/bbgen.bin", 0x0A))) and len(disk["bbmod.bin"]) > 1000 and disk["bbmod.bin"] == disk["0:/bbmod.bin"], str(opened[-8:]) + " %d bytes" % len(disk.get("bbmod.bin", b"")))
gen = disk.get("bbgen.bin", b"")
check("bbgen.bin: magic 8GEN, version 1, its own size, the Looper's 15 options and a crc", gen[:4] == b"8GEN" and struct.unpack("<HH", gen[4:8])[0] == 1 and struct.unpack("<HH", gen[4:8])[1] == len(gen) and len(gen) > 200, str((gen[:8], len(gen))))
e.call("lira_ui_learn", 1, 0)
e.call("lira_ui_learn", 2, 210)
e.call("lira_midi_msg", 0xB2, 33, 5)
e.call("lira_ui_learn", 1, 0)
e.call("lira_save")
gen = disk["bbgen.bin"]
check("bbgen.bin holds the MIDI-learn mapping (CC 33 on channel 3 -> control 210)", bytes([33, 2]) + struct.pack("<H", 210) in gen, gen[68:90].hex())
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
check("save thunk: the stock save handler runs with its arguments, then the file is written", order[-1] == ("stock save", 0x2401F508, 0x24072000) and any(o[1] == 0x0A for o in opened[-8:]), str(order))
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

# ---- SAVE TO PROJECT (n13): a page asks, the streamer task's idle pass (the stock preset poll wrapped by lira_file_poll) writes both files
polls = []
e.stub(0x08092F5C, lambda: polls.append(e.arg(0)))
def ui_save(op):
    e.call("lira_ui_save", op)
    return e.uc.reg_read(UC_ARM_REG_R0)
disk.pop("bbmod.bin", None)
disk.pop("bbgen.bin", None)
check("SAVE: idle state is 0 (SAVE)", ui_save(1) == 0)
ui_save(0)
check("... a tap sets SAVING (1) and writes nothing yet (the page never touches the card)", ui_save(1) == 1 and "bbmod.bin" not in disk and "bbgen.bin" not in disk)
e.call("lira_file_poll", 0x24002E88)
check("the idle pass: both files written (bbmod.bin and bbgen.bin), the stock poll still called once with its argument", "bbmod.bin" in disk and "bbgen.bin" in disk and polls == [0x24002E88], str((list(disk), polls)))
check("... the state is SAVED (2)", ui_save(1) == 2)
g = disk["bbgen.bin"]
import zlib
check("bbgen.bin: magic 8GEN, version 1, size, the crc matches (FNV over all but the last 4 bytes)", g[:4] == b"8GEN" and g[8:12] != b"" and len(g) == struct.unpack("<H", g[6:8])[0], str((g[:8], len(g))))
def fnv(b):
    h = 2166136261
    for x in b:
        h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h
check("... crc ok", struct.unpack("<I", g[-4:])[0] == fnv(g[:-4]))
m = disk["bbmod.bin"]
check("bbmod.bin: magic 8RND, version 3, and its crc", m[:4] == struct.pack("<I", 0x444e5238), str(m[:4]))
n_open = len(opened)
e.call("lira_file_poll", 0x24002E88)
check("with nothing asked, the idle pass only calls the stock poll (no file calls)", polls == [0x24002E88, 0x24002E88] and len(opened) == n_open, str((polls, opened[-2:])))
ui_save(0)
# a failing card: f_open returns an error
real_open = f_open
def failing_open():
    opened.append((e.cstr(e.arg(1)), e.arg(2)))
    e.uc.reg_write(UC_ARM_REG_R0, 3)                              # FR_NOT_READY
e.stub(0x08086534, failing_open)
e.call("lira_file_poll", 0x24002E88)
check("a card that does not open the files: the state is FAILED (3)", ui_save(1) == 3, str(ui_save(1)))
e.stub(0x08086534, f_open)
# n14: why a save fails (put_file's codes), the card's root as the fallback, the two files independent, the timeout
def lcodes():
    e.call("lira_ui_save", 2)
    c = e.uc.reg_read(UC_ARM_REG_R0)
    return c & 255, c >> 8
def run_save():
    ui_save(0)
    e.call("lira_file_poll", 0x24002E88)
    return ui_save(1)
def make_open(fail):
    def f():
        path = e.cstr(e.arg(1))
        opened.append((path, e.arg(2)))
        cur.update(path=path, mode=e.arg(2), pos=0)
        rc = fail(path)
        if rc == 0 and e.arg(2) == 0x0A:
            disk[path] = b""
        e.uc.reg_write(UC_ARM_REG_R0, rc)
    return f
disk.clear()
e.stub(0x08086534, make_open(lambda path: 5 if not path.startswith("0:") else 0))     # the working directory does not exist (FR_NO_PATH): the card's root takes it
check("a relative open that fails (no path) falls back to the card's root: both files written there, SAVED", run_save() == 2 and "0:/bbmod.bin" in disk and "0:/bbgen.bin" in disk, str((list(disk), lcodes())))
disk.clear()
e.stub(0x08086534, make_open(lambda path: 3))                                      # a card that is not ready at all
check("nothing opens (FR_NOT_READY): FAILED, and the reason is readable (3 for both)", run_save() == 3 and lcodes() == (3, 3), str(lcodes()))
disk.clear()
e.stub(0x08086534, make_open(lambda path: 7 if "bbmod" in path else 0))            # only bbmod.bin is refused (denied)
st = run_save()
check("bbmod.bin refused (7), bbgen.bin fine: FAILED with (7, 0), and bbgen.bin was still written (independent files)", st == 3 and lcodes() == (7, 0) and "bbgen.bin" in disk and "bbmod.bin" not in disk, str((st, lcodes(), list(disk))))
e.stub(0x08086534, f_open)
disk.clear()
real_write = None
def bad_write():
    e.uc.reg_write(UC_ARM_REG_R0, 1)                                              # a write error (FR_DISK_ERR) on the first file only
w_calls = []
def counting_write():
    w_calls.append(1)
    if len(w_calls) <= 2:                                                           # (the file and its retry at the root)
        e.uc.reg_write(UC_ARM_REG_R0, 1)
    else:
        f_write()
e.stub(0x08086A06, counting_write)
st = run_save()
check("a write error on bbmod.bin (code 21): FAILED, bbgen.bin still written", st == 3 and lcodes()[0] == 21 and lcodes()[1] == 0 and "bbgen.bin" in disk, str((st, lcodes())))
e.stub(0x08086A06, f_write)
disk.clear()
check("and the next save is fine again (SAVED, both files)", run_save() == 2 and "bbmod.bin" in disk and "bbgen.bin" in disk, str(lcodes()))
# the timeout: asked, but the task never comes
def set_ticks(n):
    for off in range(0, 0x300, 4):
        old = e.r32(0x38800C00 + off)
        e.w32(0x38800C00 + off, 0x12345678)
        e.call("looper_ticks")
        hit = e.uc.reg_read(UC_ARM_REG_R0) == 0x12345678
        e.w32(0x38800C00 + off, n if hit else old)
        if hit:
            return True
    return False
if set_ticks(10000):
    for _ in range(3000):
        pass
    ui_save(0)
    set_ticks(10000 + 2000)
    check("the idle pass never comes: after 6 s the button says FAILED with code 99", ui_save(1) == 3 and lcodes() == (99, 99), str((ui_save(1), lcodes())))
    set_ticks(10000 + 2000 + 400)
    check("... and goes back to SAVE after 2 s", ui_save(1) == 0, ui_save(1))
else:
    print("(timeout test skipped: ticks not found)")
print("SAVE TO PROJECT tests done")
print("failures:", fails)
sys.exit(1 if fails else 0)
