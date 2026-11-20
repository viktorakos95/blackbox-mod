"""SAVE TO PROJECT and the project save, round trip on a real FAT image: the STOCK FatFs runs (sdharness.py), the card is a mkfs.vfat image, and a power cycle is a fresh emulator on
the same image. Checked: the button's write (bbmod.bin and bbgen.bin, each twice: the working directory and the card's root, each read back), the files readable with mtools, a fresh
boot loading the general file from the card task's pass (MIDI learn mappings) and the project load bringing DRONE-8's settings back, the project-save hook path, no card call larger than
511 bytes, a damaged file ignored, a card that is not ready yet (retried, not given up).
NOT checked (hardware only): which directory the real tasks run in, the real streamer's scheduling, the real card driver's cache handling."""
import os
import struct
import subprocess
import sys

from unicorn.arm_const import UC_ARM_REG_R0

import sdharness as H
from emu import Emu

IMG = "out/solo+slice+duck+chord+cond+filter+cpu+od+comp+seqfix+fx2+munchi+looper+lira+cave/BLACKBOX.bin"
CARD = "out/sd/roundtrip.img"
os.makedirs("out/sd", exist_ok=True)
H.make_image(CARD, mb=64)
fails = 0


def check(name, ok, info=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + ("  " + str(info) if info else ""))
    fails += not ok


def boot():
    """a power-up: a new emulator, the card mounted by the stock FatFs, the DRONE-8 pool made as at boot"""
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
    card = H.Card(CARD, emu=e)
    e.call("interp_register", 0x24060000, 0xF1, 0, 0, 2, 0)
    return e, card


def r0(e):
    v = e.uc.reg_read(UC_ARM_REG_R0)
    return v - (1 << 32) if v & 0x80000000 else v


def mk(e):
    def call(name, *a):
        e.call(name, *a)
        return r0(e)

    def set_ticks(n):
        for off in range(0, 0x300, 4):
            old = e.r32(0x38800C00 + off)
            e.w32(0x38800C00 + off, 0x12345678)
            e.call("looper_ticks")
            hit = e.uc.reg_read(UC_ARM_REG_R0) == 0x12345678
            e.w32(0x38800C00 + off, n if hit else old)
            if hit:
                return True
        raise SystemExit("ticks not found")
    return call, set_ticks


# ---- run 1: change things, save with the button
e, card = boot()
call, set_ticks = mk(e)
set_ticks(1000)
call("lira_ui_set", 16, 321)
call("lira_ui_set", 125, 777)
call("lira_ui_set", 108, 444)
call("lira_ui_chord_set", 140, 9)
call("lira_ui_learn", 1, 0)
call("lira_ui_learn", 2, 210)
call("lira_midi_msg", 0xB2, 33, 5)
call("lira_ui_learn", 2, 304)
call("lira_midi_msg", 0x91, 62, 100)
call("lira_ui_learn", 1, 0)
want = (call("lira_ui_get", 16), call("lira_ui_get", 125), call("lira_ui_get", 108), call("lira_ui_chord_get", 140))
check("run 1: state set (CC 33 on channel 3 -> control 210, note 62 on channel 2 -> plate 5)", want == (321, 777, 444, 9) and call("lira_ui_learn", 0) >> 8 == 2, want)
card.log.clear()
call("lira_ui_save", 0)
check("the button asks: SAVING", call("lira_ui_save", 1) == 1)
e.call("lira_save_service")
check("the card task's pass writes it: SAVED", call("lira_ui_save", 1) == 2, (call("lira_ui_save", 1), call("lira_ui_save", 2)))
card.save()
ls = subprocess.run(["mdir", "-i", CARD, "::/"], capture_output=True, text=True).stdout
check("the card has bbmod.bin and bbgen.bin in its root (the working directory is the root here)", "BBMOD" in ls.upper() and "BBGEN" in ls.upper(), ls)
fs = subprocess.run(["fsck.vfat", "-n", CARD], capture_output=True, text=True)
check("fsck.vfat is clean after the writes", fs.returncode == 0, fs.stdout[-200:])
out = "out/sd/rt_bbgen.bin"
subprocess.run(["mcopy", "-n", "-i", CARD, "::/bbgen.bin", out], check=True)
g = open(out, "rb").read()
check("bbgen.bin on the card: magic 8GEN, version 1, its size, a good crc", g[:4] == b"8GEN" and struct.unpack("<HH", g[4:8]) == (1, len(g)), (g[:8], len(g)))

# ---- run 2: a power cycle. The card image is re-read; nothing of run 1's RAM is left
e2, card2 = boot()
call2, set_ticks2 = mk(e2)
set_ticks2(300)
check("after the power cycle the settings are at their defaults again", call2("lira_ui_get", 16) != 321 and call2("lira_ui_learn", 0) >> 8 == 0)
e2.call("lira_save_service")
check("the card task's first pass loads the general file: the two MIDI-learn mappings are back", call2("lira_ui_learn", 0) >> 8 == 2 and call2("lira_ui_learn", 5, 210) == 34 and call2("lira_ui_learn", 5, 304) == 63, (call2("lira_ui_learn", 0), call2("lira_ui_learn", 5, 210)))
check("... and a mapped CC works at once (CC 33 on channel 3 sets control 210)", (call2("lira_midi_msg", 0xB2, 33, 127), call2("lira_ui_get", 210))[1] == 1000, call2("lira_ui_get", 210))
call2("lira_load")
got = (call2("lira_ui_get", 16), call2("lira_ui_get", 125), call2("lira_ui_get", 108), call2("lira_ui_chord_get", 140))
check("the project load brings DRONE-8's settings back (bbmod.bin)", got == want, got)

# ---- a card that is not ready at the first pass: the general file is retried, not given up
e5, card5 = boot()
call5, set_ticks5 = mk(e5)
set_ticks5(300)
good_read = e5.stubs[H.DRV_READ]
def bad_read():
    e5.ret(1)                                                       # RES_ERROR
e5.stubs[H.DRV_READ] = bad_read
card5.FS_dirty = True
e5.uc.mem_write(card5.FS + 0x00, b"\x00")                           # (the mounted volume forgets its window: the next call has to read)
e5.call("lira_save_service")
check("card not ready (read errors): the general file is not loaded and not given up", call5("lira_ui_learn", 0) >> 8 == 0)
e5.stubs[H.DRV_READ] = good_read
set_ticks5(300 + 100)
e5.call("lira_save_service")
check("... a pass less than a second later does not try again (throttled)", call5("lira_ui_learn", 0) >> 8 == 0)
set_ticks5(300 + 400)
e5.call("lira_save_service")
check("... and the next try after a second loads it", call5("lira_ui_learn", 0) >> 8 == 2, call5("lira_ui_learn", 0))

# ---- the project-save hook path writes the same files (a different change, the stock save's hook)
call2("lira_ui_set", 16, 555)
e2.call("lira_save")
card2.save()
e3, card3 = boot()
call3, set_ticks3 = mk(e3)
set_ticks3(300)
e3.call("lira_load")
check("the project-save hook's file loads after a power cycle too (control 16 = 555), and the general file with it", call3("lira_ui_get", 16) == 555 and call3("lira_ui_learn", 0) >> 8 == 2, (call3("lira_ui_get", 16), call3("lira_ui_learn", 0)))

# ---- a damaged file is ignored
card3.disk[:] = card3.disk
subprocess.run(["mcopy", "-i", CARD, "-o", "-n", "/dev/null", "::/bbgen.bin"], capture_output=True)
e4, card4 = boot()
call4, set_ticks4 = mk(e4)
set_ticks4(300)
e4.call("lira_save_service")
check("an empty / damaged general file is ignored (no mappings), and does not stop the boot", call4("lira_ui_learn", 0) >> 8 == 0)
print("failures:", fails)
sys.exit(1 if fails else 0)
