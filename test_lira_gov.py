"""The load governor of the DRONE synth (lira_pad.c govern, lira_page_run) under Unicorn.

The audio task's wake-up time (0x2405ffd4) and the cycle counter are written by the test: a block is 2.56 M cycles, and the stock
work done by the time the synth runs is 50 % / 85 % / 97 % of it. Checked: no step at 50 %, a step every ~16 blocks while the load
stays over 80 % (one delay line, no delay, silence), the synth stays silent, and steps back after ~400 blocks with room again.
NOT checked: the real cycle counts on the device.
"""
import math
import struct

from unicorn.arm_const import UC_ARM_REG_R0

from emu import Emu

IMG = "out/solo+slice+duck+chord+cond+filter+cpu+od+comp+seqfix+fx2+munchi+lira+cave/BLACKBOX.bin"
e = Emu(IMG, "out/cave.elf")
e.uc.mem_map(0xC0000000, 0x400000)
e.uc.mem_map(0x38800000, 0x1000)
e.uc.mem_map(0x58024000, 0x1000)
e.uc.mem_map(0xE0000000, 0x10000)
fails = 0


def check(name, ok, info=""):
    global fails
    info = str(info) if info != "" else ""
    print(("PASS " if ok else "FAIL ") + name + ("  " + info if info else ""))
    fails += not ok


e.stub(0x0808C0D8)
e.stub(0x0808C12C)
e.stub(0x0804436C, value=0xC0000000)
e.w32(0x24000088, 0xC4000000)
e.w32(0x2400008C, 0xC0000000)
e.call("interp_register", 0x24060000, 0xF1, 0, 0, 2, 0)
N, PER = 256, 2_560_000
L, R = 0x24070000, 0x24071000
cyc = [10_000_000]


def block(load):
    cyc[0] += PER
    e.w32(0xE0001004, cyc[0] & 0xFFFFFFFF)
    e.w32(0x2405FFD4, (cyc[0] - int(PER * load)) & 0xFFFFFFFF)
    e.uc.mem_write(L, bytes(4 * N))
    e.uc.mem_write(R, bytes(4 * N))
    e.call("lira_page_run", L, R, N)
    x = struct.unpack(f"<{N}f", bytes(e.uc.mem_read(L, 4 * N)))
    return math.sqrt(sum(v * v for v in x) / N)


def shed():
    e.call("lira_ui_load", 2)
    return e.uc.reg_read(UC_ARM_REG_R0)


# Touch = Ext with no plate held: the engine must run (the input opens the voices) instead of waiting for a plate
e.call("lira_ui_set", 100, 2)
e.call("lira_ui_set", 101, 2)
e.call("lira_ui_set", 102, 2)
e.call("lira_ui_set", 103, 2)
e.call("lira_ui_action", 5)                                     # X:MIX
e.call("lira_ui_set", 121, 0)
e.call("lira_ui_set", 122, 0)


def block_in(level, x):
    cyc[0] += PER
    e.w32(0xE0001004, cyc[0] & 0xFFFFFFFF)
    e.w32(0x2405FFD4, (cyc[0] - int(PER * level)) & 0xFFFFFFFF)
    e.uc.mem_write(L, struct.pack(f"<{N}f", *([x] * N)))
    e.uc.mem_write(R, struct.pack(f"<{N}f", *([x] * N)))
    e.call("lira_page_run", L, R, N)
    y = struct.unpack(f"<{N}f", bytes(e.uc.mem_read(L, 4 * N)))
    return math.sqrt(sum((a - x) ** 2 for a in y) / N)


for _ in range(60):
    r = block_in(0.3, 0.5)
check("Touch = Ext: with no plate held the synth still sounds from the input", r > 0.001, "rms %.5f" % r)
e.call("lira_ui_set", 100, 1)
e.call("lira_ui_set", 101, 1)
e.call("lira_ui_set", 102, 1)
e.call("lira_ui_set", 103, 1)
e.call("lira_ui_key", 0, 1)
e.call("lira_ui_key", 3, 1)
for _ in range(40):
    block(0.5)
r = block(0.5)
check("50 % load: full quality, the synth sounds", shed() == 0 and r > 0.001, "shed %d rms %.4f" % (shed(), r))
seen = []
for i in range(120):
    r = block(0.85)
    if not seen or seen[-1] != shed():
        seen.append(shed())
check("85 % load: it steps 1, 2, 3 (one line, none, muted)", seen == [0, 1, 2, 3] or seen == [1, 2, 3], str(seen))
check("... and is silent at step 3", r == 0.0, "rms %.5f" % r)
for _ in range(300):
    block(0.5)
check("room again, but not for long enough: still muted", shed() == 3, str(shed()))
for _ in range(150):
    block(0.5)
check("after ~400 blocks it steps back one", shed() == 2, str(shed()))
for _ in range(900):
    block(0.5)
check("... and all the way back to full quality", shed() == 0 and block(0.5) > 0.001, str(shed()))

# ---- things the page does around the engine ----
def rv(name, *args):
    e.call(name, *args)
    v = e.uc.reg_read(UC_ARM_REG_R0)
    return v - (1 << 32) if v & 0x80000000 else v


for k in range(8):
    e.call("lira_ui_key", k, 0)
for _ in range(10):
    block(0.4)
# chord on / off keeps the knobs
e.call("lira_ui_action", 0)
on_pos = [rv("lira_ui_get", v) for v in range(8)]
e.call("lira_ui_action", 0)
off_pos = [rv("lira_ui_get", v) for v in range(8)]
check("chord mode off keeps the voices' knob positions (where the chord put them)", all(abs(a - b) <= 1 for a, b in zip(on_pos, off_pos)) and len(set(off_pos)) > 3, "%s -> %s" % (on_pos, off_pos))
check("... and the Pitch knobs sit at unity, so nothing changes in the sound", abs(rv("lira_ui_get", 123) - 497) <= 1 and abs(rv("lira_ui_get", 124) - 497) <= 1, (rv("lira_ui_get", 123), rv("lira_ui_get", 124)))
# a chord voice goes the oscillator's whole range
e.call("lira_ui_pitch_set", 0, -5000)
e.call("lira_ui_pitch_set", 7, 30000)
check("a voice goes below A1 / above A6 as far as its oscillator reaches (voice 1 down to -16, voice 8 up to MIDI 127)", rv("lira_ui_pitch", 0) == -1599 and rv("lira_ui_pitch", 7) == 12699, (rv("lira_ui_pitch", 0), rv("lira_ui_pitch", 7)))
e.call("lira_ui_pitch_set", 0, 6050)
check("... a half-way pitch is stored as the nearer note and its cents", rv("lira_ui_note", 0) == 61 and rv("lira_ui_pitch", 0) == 6050, (rv("lira_ui_note", 0), rv("lira_ui_pitch", 0)))
# tempo sync: the knobs are stored and the engine keeps sounding (no clock here: the 120 bpm fallback)
e.call("lira_ui_chord_set", 144, 6)
e.call("lira_ui_chord_set", 146, 8)
check("the sync knobs are stored (LFO A 1/4, delay 1 1/8)", rv("lira_ui_chord_get", 144) == 6 and rv("lira_ui_chord_get", 146) == 8)
e.call("lira_ui_key", 0, 1)
for _ in range(60):
    r = block(0.3)
check("with sync on the synth still sounds", r > 0.001, "rms %.5f" % r)
e.call("lira_ui_chord_set", 144, 0)
e.call("lira_ui_chord_set", 146, 0)
# the gesture recorder (no looper here: it starts at once, and a bar is 4 beats of the 120 bpm fallback = 96000 frames)
e.call("lira_ui_key", 0, 0)
for _ in range(20):
    block(0.3)
e.call("lira_ui_action", 7)
check("REC arms the recorder", rv("lira_ui_chord_get", 158) == 1)
block(0.3)
check("... and with no clock it records from the next block", rv("lira_ui_chord_get", 158) == 2, rv("lira_ui_chord_get", 158))
for _ in range(10):
    block(0.3)
e.call("lira_ui_set", 120, 400)
for _ in range(10):
    block(0.3)
e.call("lira_ui_pitch_set", 3, 5000)
for _ in range(360):
    block(0.3)
check("after LEN bars (1) the take loops: %d gestures, 1 layer" % rv("lira_ui_chord_get", 159), rv("lira_ui_chord_get", 158) == 3 and rv("lira_ui_chord_get", 159) == 2 and rv("lira_ui_chord_get", 149) == 1, (rv("lira_ui_chord_get", 158), rv("lira_ui_chord_get", 159), rv("lira_ui_chord_get", 149)))
e.call("lira_ui_set", 120, 700)
e.call("lira_ui_pitch_set", 3, 7000)
lvl = pit = 0
for _ in range(400):
    block(0.3)
    lvl |= rv("lira_ui_get", 120) == 400
    pit |= rv("lira_ui_pitch", 3) == 5000
check("the loop plays the level and the pitch again", lvl and pit, (lvl, pit))
e.call("lira_ui_action", 7)
check("REC in the loop arms an overdub from the loop start", rv("lira_ui_chord_get", 158) == 4)
for _ in range(400):
    block(0.3)
    if rv("lira_ui_chord_get", 158) == 5:
        break
check("... which records for one pass", rv("lira_ui_chord_get", 158) == 5, rv("lira_ui_chord_get", 158))
e.call("lira_ui_set", 121, 300)
for _ in range(400):
    block(0.3)
    if rv("lira_ui_chord_get", 158) == 3:
        break
check("... and then it is a second layer (3 gestures)", rv("lira_ui_chord_get", 158) == 3 and rv("lira_ui_chord_get", 149) == 2 and rv("lira_ui_chord_get", 159) == 3, (rv("lira_ui_chord_get", 158), rv("lira_ui_chord_get", 149), rv("lira_ui_chord_get", 159)))
e.call("lira_ui_action", 9)
check("UNDO drops the last layer", rv("lira_ui_chord_get", 149) == 1 and rv("lira_ui_chord_get", 159) == 2, (rv("lira_ui_chord_get", 149), rv("lira_ui_chord_get", 159)))
e.call("lira_ui_action", 14)
check("CLEAR empties it", rv("lira_ui_chord_get", 158) == 0 and rv("lira_ui_chord_get", 159) == 0)
check("the flight recorder in backup SRAM holds this run's peaks", e.r32(0x38800BC0) == 0x46524331 and e.r16(0x38800BC4) >= 840, str((hex(e.r32(0x38800BC0)), e.r16(0x38800BC4))))
e.call("lira_ui_prev", 4)
check("... no run before it yet", e.uc.reg_read(UC_ARM_REG_R0) == 0)
e.call("interp_register", 0x24060000, 0xF1, 0, 0, 2, 0)                  # the next boot
e.call("lira_ui_prev", 4)
ok = e.uc.reg_read(UC_ARM_REG_R0)
e.call("lira_ui_prev", 0)
peak = e.uc.reg_read(UC_ARM_REG_R0)
check("the next boot keeps them as the run before (peak load %)", ok == 1 and 84 <= peak <= 99, "valid %d peak %d" % (ok, peak))
print("failures:", fails)
raise SystemExit(1 if fails else 0)
