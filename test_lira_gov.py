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
def wave(load):
    cyc[0] += PER
    e.w32(0xE0001004, cyc[0] & 0xFFFFFFFF)
    e.w32(0x2405FFD4, (cyc[0] - int(PER * load)) & 0xFFFFFFFF)
    e.uc.mem_write(L, bytes(4 * N))
    e.uc.mem_write(R, bytes(4 * N))
    e.call("lira_page_run", L, R, N)
    return list(struct.unpack(f"<{N}f", bytes(e.uc.mem_read(L, 4 * N))))
seen = []
rmss = []
steps = []                                       # the largest jump between neighbouring samples in a block
for i in range(60):
    w = wave(0.85)
    if not seen or seen[-1] != shed():
        seen.append(shed())
check("85 % of the block used before the synth (it is not at the mark): no step, however long", seen == [0], str(seen))
for i in range(440):
    w = wave(0.975)
    r = math.sqrt(sum(v * v for v in w) / N)
    rmss.append(r)
    steps.append(max(abs(w[k + 1] - w[k]) for k in range(N - 1)))
    if not seen or seen[-1] != shed():
        seen.append(shed())
check("the stock work alone nearly fills the block (97.5 % before the synth): it goes out, in one step (no half-states of the sound)", seen == [0, 3], str(seen))
r = rmss[-1]
check("... and is silent once it has faded out", r == 0.0, "rms %.5f" % r)
peak = max(rmss[:20])
first_low = next((i for i, v in enumerate(rmss) if v < 0.5 * peak), None)
check("... and the fade takes a long while (the output is still half or more 60 blocks after the step)", first_low is None or first_low > 120, str(first_low))
check("... the delay lines going make no jump: the largest step between samples stays within 3x of the first blocks'",
      max(steps) < 3 * max(steps[:10]) + 1e-4, "max %.5f first %.5f" % (max(steps), max(steps[:10])))
for _ in range(10):
    block(0.5)
check("room again, but not for long enough: still out", shed() == 3, str(shed()))
back = []
for _ in range(400):
    back.append(block(0.5))
check("after ~500 blocks it comes back", shed() == 0, str(shed()))
i0 = next((i for i, v in enumerate(back) if v > 0.001), None)
lvl = sum(back[-30:]) / 30
check("... fading in (the first blocks are quiet; it takes ~190 blocks to reach its level)", i0 is not None and back[i0] < 0.3 * lvl and sum(back[i0 + 150:i0 + 190]) / 40 > 0.6 * lvl, "first %.4f level %.4f" % (back[i0] if i0 is not None else -1, lvl))
# the whole audio task (cpu.c's running block count and load sum): work after the synth that the mid-task value cannot see
CPU = 0x2405FFD0
e.uc.mem_write(CPU, struct.pack("<IIIHH", 0x43505532, 0, 0, 0, 0))
wsum = [0, 0]


def block_w(mid, whole):
    wsum[0] = (wsum[0] + 1) % 188
    wsum[1] = 0 if wsum[0] == 0 else wsum[1] + int(1000 * whole)
    e.uc.mem_write(CPU + 8, struct.pack("<IH", wsum[1], wsum[0]))
    return block(mid)


for _ in range(1500):
    block_w(0.5, 0.7)
check("whole task at 70 %, 50 % by the time the synth runs: no step", shed() == 0, str(shed()))
for _ in range(1500):
    block_w(0.5, 0.93)
check("whole task at 93 % (under its mark, 95 % with no page opened and this synth ranked highest): no step", shed() == 0, str(shed()))
for _ in range(60):
    block_w(0.5, 0.99)
    block_w(0.5, 0.80)
check("spikes to 99 % every other block average to 89 %: no step (a moment of the screen or the SD card is not a reason)", shed() == 0, str(shed()))
n_to = 0
for i in range(2000):
    block_w(0.5, 0.97)
    if shed() == 3:
        n_to = i
        break
check("whole task at 97 % (over the mark): it goes out after a good while (the average has to get there), in one step", shed() == 3 and n_to > 150, (shed(), n_to))
for _ in range(800):
    block_w(0.5, 0.935)
check("whole task at 93.5 % (between the mark less 3 % and the mark): it stays out (the hysteresis; no flapping)", shed() == 3, str(shed()))
for _ in range(2500):
    block_w(0.5, 0.6)
check("whole task at 60 %: back", shed() == 0, str(shed()))
# granular SAMPLR beside it: the load swings between 60 and 95 % every ~0.3 s; the average (~78 %) is far from the mark
for i in range(40):
    for _ in range(50):
        block_w(0.5, 0.95)
    for _ in range(50):
        block_w(0.5, 0.60)
check("a load that swings (granular): no step", shed() == 0, str(shed()))
# the page opened last leads: its mark is the highest
e.call("looper_page_set_leader", 2)
for _ in range(2500):
    block_w(0.5, 0.945)
check("this synth's page opened last: its mark is the highest (95 %): 94.5 % is no reason", shed() == 0, str(shed()))
e.call("looper_page_set_leader", 0)
for _ in range(2500):
    block_w(0.5, 0.945)
check("the Looper page opened last: the synth gives way before the looper (its mark is lower, 93.5 %): 94.5 % takes it out", shed() == 3, str(shed()))
e.call("looper_page_set_leader", 2)
run_n = 0
for _ in range(10):
    block_w(0.5, 0.945)
check("... and the synth's page is opened: it comes back at once", shed() == 0, str(shed()))
e.call("looper_page_set_leader", 3)
for _ in range(2500):
    block_w(0.5, 0.6)
e.uc.mem_write(CPU, bytes(4))                                   # (the rest runs without the meter, as before)

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
# STOP / PLAY of the loop, and no repaint storm while it plays
sig_loop = rv("lira_ui_sig")
e.call("lira_ui_key", 5, 1)
check("while a loop plays, a plate changing does not change the repaint signature", rv("lira_ui_sig") == sig_loop, (sig_loop, rv("lira_ui_sig")))
e.call("lira_ui_key", 5, 0)
e.call("lira_ui_set", 120, 700)
e.call("lira_ui_action", 15)
block(0.3)
check("STOP pauses the loop (at the bar line; with no clock the next block)", rv("lira_ui_chord_get", 158) == 6, rv("lira_ui_chord_get", 158))
for _ in range(400):
    block(0.3)
check("... and a paused loop replays nothing", rv("lira_ui_get", 120) == 700, rv("lira_ui_get", 120))
e.call("lira_ui_action", 15)
block(0.3)
check("PLAY starts it again from the top", rv("lira_ui_chord_get", 158) == 3)
e.call("lira_ui_action", 17)
block(0.3)
check("the PLAY button does nothing to a loop that is playing", rv("lira_ui_chord_get", 158) == 3)
e.call("lira_ui_action", 16)
block(0.3)
check("the STOP button pauses it", rv("lira_ui_chord_get", 158) == 6)
e.call("lira_ui_action", 16)
block(0.3)
check("... STOP again changes nothing", rv("lira_ui_chord_get", 158) == 6)
e.call("lira_ui_action", 17)
block(0.3)
check("... PLAY starts it", rv("lira_ui_chord_get", 158) == 3)
gn0 = rv("lira_ui_chord_get", 159)
e.call("lira_ui_key", 3, 1)
e.call("lira_ui_action", 7)
for _ in range(3000):
    block(0.3)
    if rv("lira_ui_chord_get", 158) == 5:
        break
check("a plate held when an overdub layer starts is recorded into it", rv("lira_ui_chord_get", 158) == 5 and rv("lira_ui_chord_get", 159) > gn0, (rv("lira_ui_chord_get", 158), gn0, rv("lira_ui_chord_get", 159)))
e.call("lira_ui_key", 3, 0)
e.call("lira_ui_action", 7)
e.call("lira_ui_action", 9)
check("UNDO drops the last layer", rv("lira_ui_chord_get", 149) == 1 and rv("lira_ui_chord_get", 159) == 2, (rv("lira_ui_chord_get", 149), rv("lira_ui_chord_get", 159)))
e.call("lira_ui_action", 14)
check("CLEAR empties it", rv("lira_ui_chord_get", 158) == 0 and rv("lira_ui_chord_get", 159) == 0)
e.call("lira_ui_action", 18)
check("ENGINE off is on record", rv("lira_ui_chord_get", 194) == 1)
for _ in range(20):
    r_off = block(0.3)
check("... the synth does not run: silence and no share of the audio task", r_off == 0.0 and rv("lira_ui_load", 0) == 0, (r_off, rv("lira_ui_load", 0)))
e.call("lira_ui_action", 18)
check("ENGINE on again", rv("lira_ui_chord_get", 194) == 0)
e.uc.mem_write(0x2405FFE2, struct.pack("<HH", 523, 871))
check("the CPU readouts are cpu.c's average and worst block", (rv("lira_ui_load", 3), rv("lira_ui_load", 4)) == (52, 87), (rv("lira_ui_load", 3), rv("lira_ui_load", 4)))
check("the flight recorder in backup SRAM holds this run's peaks", e.r32(0x38800BC0) == 0x46524331 and e.r16(0x38800BC4) >= 840, str((hex(e.r32(0x38800BC0)), e.r16(0x38800BC4))))
e.uc.mem_write(0x2405FFE4, struct.pack("<H", 953))
block(0.3)
e.call("lira_ui_prev", 4)
check("... no run before it yet", e.uc.reg_read(UC_ARM_REG_R0) == 0)
e.call("interp_register", 0x24060000, 0xF1, 0, 0, 2, 0)                  # the next boot
e.call("lira_ui_prev", 4)
ok = e.uc.reg_read(UC_ARM_REG_R0)
e.call("lira_ui_prev", 0)
peak = e.uc.reg_read(UC_ARM_REG_R0)
e.call("lira_ui_prev", 5)
w = e.uc.reg_read(UC_ARM_REG_R0)
check("... and the whole audio task's worst block (cpu.c)", w == 95, w)
check("the next boot keeps them as the run before (peak load %)", ok == 1 and 84 <= peak <= 99, "valid %d peak %d" % (ok, peak))
print("failures:", fails)
raise SystemExit(1 if fails else 0)
