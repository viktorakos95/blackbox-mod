"""Trigger2MIDI wired into the firmware (src/t2m_bb.c, src/t2m_thunk.S, called from src/looper.c), under Unicorn on the
patched image.

Real code: looper_in (the input stage's tail call) with the looper idle, t2m_boot, t2m_audio, the detector, the stock
engine note-on / note-off builders (FUN_0804c61c / FUN_0804c5d4, through the learn thunk at the first one's entry).
Stubbed: the SDRAM allocator and its free-space read, the stock MIDI send FUN_08044a0e (recorded), the engine's command
dispatcher FUN_080520ec (recorded), the engine command queue writer FUN_08051738 (recorded), the input stage tail.
Checked: memory taken once at boot (and not at all when the allocator is short), the MIDI messages of a hit signal on
input L are the reference's (t2m_ref.py) and go to the stock port manager with the right TRS / USB flags, input R is
silent while off and follows its own port when on, switching an input off ends its sounding note; the pad target: LEARN
takes the pad of the next stock note-on and only then, the 24-byte command we dispatch is byte for byte the one the stock
builder queues, note-offs, nothing when the engine is not up; Note Out OFF keeps the hits and the pad.

python3 patch.py solo slice duck chord cond filter cpu od comp seqfix fx2 munchi looper cave && python3 test_t2m_bb.py
"""
import os
import random
import struct
import subprocess
import sys
from math import exp, pi, sin

import unicorn.arm_const as A

import t2m_ref as ref
from emu import STACK, Emu

IMG = "out/solo+slice+duck+chord+cond+filter+cpu+od+comp+seqfix+fx2+munchi+looper+cave/BLACKBOX.bin"
SR, N = 48000, 256
MGR = 0x24002E88
ROOT = 0x38800980
MEM = 0x30100000
INL, INR, PTRS = 0x30010000, 0x30011000, 0x30012000
ENGINE, OBJ = 0x2400A9C0, 0x30050000
MAGIC = 0x54324D32

fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


# struct offsets from the header itself (same layout on the host: only 1-, 2- and 4-byte fields)
FIELDS = ["on", "port", "last_vel", "last_note", "hits", "peak", "blocks", "hit_blk", "pad_on", "pad_note", "pad_held",
          "learn", "pad_id", "pad_sent", "hist_n", "hist", "ui", "p", "st"]
os.makedirs("out/t2m_test", exist_ok=True)
open("out/t2m_test/off.c", "w").write(
    '#include <stdio.h>\n#include <stddef.h>\n#include "../../src/t2m_bb.h"\nint main(void){\n'
    + "".join(f'printf("{f} %zu\\n", offsetof(struct t2m_mem, {f}));\n' for f in FIELDS)
    + 'printf("SIZE %zu\\nPSIZE %zu\\nHIT %zu\\n", sizeof(struct t2m_mem), sizeof(t2m_params), sizeof(struct t2m_hit));\n'
    + "".join(f'printf("p_{f} %zu\\n", offsetof(t2m_params, {f}));\n' for f in ("note", "note_on", "vel_mode", "vel_fixed"))
    + "}\n")
subprocess.run(["gcc", "-o", "out/t2m_test/off", "out/t2m_test/off.c"], check=True)
O = {l.split()[0]: int(l.split()[1]) for l in
     subprocess.run(["out/t2m_test/off"], capture_output=True, text=True, check=True).stdout.splitlines()}
SIZE, PSIZE = O["SIZE"], O["PSIZE"]

e = Emu(IMG, "out/cave.elf")
e.uc.mem_map(0x38800000, 0x1000)                     # backup SRAM
e.uc.mem_map(0x58024000, 0x1000)                     # RCC / PWR
e.uc.mem_map(0xE0001000, 0x1000)                     # DWT
e.uc.mem_map(0xE000E000, 0x1000)                     # DEMCR (the clock monitor switches the cycle counter on)
e.uc.mem_map(0x30000000, 0x200000)                   # buffers + the "SDRAM" block the allocator hands out

free_bytes = [3 << 20]
allocs, sent, dispatched, queued = [], [], [], []
e.stub(0x080443F0, fn=lambda: e.uc.reg_write(A.UC_ARM_REG_R0, free_bytes[0]))


def alloc():
    allocs.append(e.arg(0))
    e.uc.reg_write(A.UC_ARM_REG_R0, MEM)


e.stub(0x08044404, fn=alloc)
e.stub(0x08044A0E, fn=lambda: sent.append((e.arg(0), bytes(e.uc.mem_read(e.arg(1), 5)), e.arg(2), e.arg(3))))
e.stub(0x080520EC, fn=lambda: dispatched.append((e.arg(0), bytes(e.uc.mem_read(e.arg(1), 24)))))
e.stub(0x08051738, fn=lambda: queued.append((e.arg(0), bytes(e.uc.mem_read(e.arg(1), 24)))))
e.stub(0x080518F0)                                    # the input stage's own tail (FUN_080518f0)
e.w32(PTRS, INL)
e.w32(PTRS + 4, INR)
e.w32(ENGINE + 0x9000 + 0xA74, OBJ)                   # the engine object the command queue lives in


def feed(left, right):
    """Blocks through looper_in (the real hook), as the audio task does."""
    out = []
    for b in range(0, len(left), N):
        e.uc.mem_write(INL, struct.pack(f"<{N}f", *(left[b:b + N] + [0.0] * (N - len(left[b:b + N])))))
        e.uc.mem_write(INR, struct.pack(f"<{N}f", *(right[b:b + N] + [0.0] * (N - len(right[b:b + N])))))
        sent.clear()
        e.call("looper_in", 0x30000000, PTRS, N, count=5_000_000)
        out += [(b, s) for s in sent]
    return out


def msgs(out):
    return [(m[0], m[1], m[2]) for _, (mgr, m, uart, usb) in out]


def stock(fn_addr, pad, note, a4, a5):
    """The stock engine note-on / off builder: the command it queues."""
    queued.clear()
    e.uc.mem_write(STACK, struct.pack("<I", a5))      # the fifth argument, on the stack
    e.call(fn_addr, ENGINE, pad, note, a4)
    return list(queued)


# ---- boot
e.call("t2m_boot")
check(f"boot: one allocation of the settings + two detectors ({SIZE} bytes)", allocs == [SIZE], allocs)
check("boot: the root in the backup SRAM points at it", e.r32(ROOT) == MAGIC and e.r32(ROOT + 4) == MEM,
      (hex(e.r32(ROOT)), hex(e.r32(ROOT + 4))))
check("boot: input L on, R off, both on TRS + USB, no pad target",
      [e.r8(MEM + O["on"]), e.r8(MEM + O["on"] + 1), e.r8(MEM + O["port"]), e.r8(MEM + O["port"] + 1),
       e.r8(MEM + O["pad_on"]), e.r8(MEM + O["pad_on"] + 1)] == [1, 0, 3, 3, 0, 0])

rng = random.Random(3)


def drum(amp, n=int(SR * 0.12)):
    hz, ph = rng.uniform(80, 200), rng.uniform(0, 2 * pi)
    return [amp * min(1, (i + 1) / 24) * (0.5 * sin(ph + 2 * pi * hz * i / SR) * exp(-i / (SR * 0.06)) +
                                          0.5 * rng.uniform(-1, 1) * exp(-i / (SR * 0.006))) for i in range(n)]


sig = [0.0] * (SR * 2)
for k in range(12):
    for i, x in enumerate(drum([0.8, 0.1, 0.4, 0.05][k % 4])):
        if k * 6000 + 300 + i < len(sig):
            sig[k * 6000 + 300 + i] += x
sig = [struct.unpack("<f", struct.pack("<f", max(-1, min(1, x))))[0] for x in sig]
want = [(st, d1, d2) for _, st, d1, d2 in ref.run(sig, ref.DEFAULTS)]
silence = [0.0] * len(sig)

out = feed(sig, silence)
check(f"input L: the reference's {len(want)} messages ({sum(1 for m in want if m[0] == 0x90)} notes), same order and values",
      msgs(out) == want, (msgs(out)[:6], want[:6]))
check("input L: every message to the stock port manager, 3 bytes, on TRS and USB",
      all(mgr == MGR and m[4] == 3 and uart and usb for _, (mgr, m, uart, usb) in out), out[:2])
check("input L: each message in the block its sample is in",
      all(b <= f < b + N for (b, _), f in zip(out, [f for f, *_ in ref.run(sig, ref.DEFAULTS)])))
notes = sum(1 for m in want if m[0] == 0x90)
check(f"page counters: {e.r32(MEM + O['hits'])} hits, last velocity {e.r8(MEM + O['last_vel'])}, last note {e.r8(MEM + O['last_note'])}",
      e.r32(MEM + O["hits"]) == notes and e.r8(MEM + O["last_note"]) == 38)
hn = e.r8(MEM + O["hist_n"])
hist = [struct.unpack_from("<IBBxx", bytes(e.uc.mem_read(MEM + O["hist"] + 8 * i, 8))) for i in range(24)]
check("history: one entry per hit, with its velocity and note, in order", hn == notes % 24 and
      [h[1] for h in hist[:notes]] == [m[2] for m in want if m[0] == 0x90] and all(h[2] == 38 for h in hist[:notes]), (hn, hist[:4]))
check("no pad target: nothing dispatched to the engine, no stock command queued", dispatched == [] and queued == [])

out = feed(silence, sig)
check("input R off: nothing sent for hits on R", out == [], out[:3])

# input R on, USB only, its own note
e.uc.mem_write(MEM + O["on"] + 1, b"\x01")
e.uc.mem_write(MEM + O["port"] + 1, b"\x02")
e.uc.mem_write(MEM + O["p"] + PSIZE + O["p_note"], bytes([45]))
out = feed(silence, sig)
check("input R on: the same hits, on its own pitch (45)", [m[1] for m in msgs(out)] == [45 if m[1] == 38 else m[1] for m in want],
      msgs(out)[:4])
check("input R on USB only: uart flag 0, usb flag set", out and all(not uart and usb for _, (_, _, uart, usb) in out), out[:2])

out = feed(sig, sig)
check("both inputs: twice the messages", len(out) == 2 * len(want), (len(out), len(want)))
e.uc.mem_write(MEM + O["on"] + 1, b"\x00")

# switching L off while its Dynamic note is held ends that note
out = feed(sig[:900], silence[:900])
on_notes = [m for m in msgs(out) if m[0] == 0x90]
e.uc.mem_write(MEM + O["on"], b"\x00")
out = feed(silence[:N], silence[:N])
check("input L switched off with a note held: its note-off goes out", on_notes and msgs(out) == [(0x80, 38, 0)], msgs(out))
out = feed(sig, silence)
check("input L off: silent", out == [])
e.uc.mem_write(MEM + O["on"], b"\x01")

# ---- the pad target
# LEARN: not armed, a stock pad hit changes nothing (and the stock builder still runs: the thunk continues into it)
PAD, NOTE = 0x00010203, 60
q = stock(0x0804C61C, PAD, NOTE, 0, 90)
check("stock note-on through the thunk: the stock command is still queued", len(q) == 1 and q[0][1][0] == 0x42, q)
GOLD_ON = q[0][1]
check("... the stock command layout: id 0x42, pad at +8, note at +0xc, a4 at +0x10, half 1 at +0x14, velocity half at +0x16",
      GOLD_ON[:8] == bytes([0x42, 0, 0, 0, 0, 0, 0, 0]) and struct.unpack_from("<IIIHH", GOLD_ON, 8) == (PAD, NOTE, 0, 1, 90), GOLD_ON.hex())
check("not armed: nothing learned", e.r8(MEM + O["pad_on"]) == 0 and e.r32(MEM + O["pad_id"]) == 0)
q = stock(0x0804C61C, PAD, 200, 0, 90)
check("a note above 127 is dropped by the stock builder as before", q == [], q)

e.uc.mem_write(MEM + O["learn"], b"\x01")                     # LEARN armed for input L
q = stock(0x0804C61C, PAD, 62, 0, 90)
check("LEARN armed: the next stock note-on teaches input L its pad and note, switches the target on, disarms",
      e.r32(MEM + O["pad_id"]) == PAD and e.r8(MEM + O["pad_note"]) == 62 and e.r8(MEM + O["pad_on"]) == 1 and e.r8(MEM + O["learn"]) == 0
      and e.r32(MEM + O["pad_id"] + 4) == 0, (hex(e.r32(MEM + O["pad_id"])), e.r8(MEM + O["pad_note"])))
check("... and the stock command is still queued", len(q) == 1 and q[0][1][0] == 0x42)
stock(0x0804C61C, 0x777, 50, 0, 90)
check("a second pad hit teaches nothing more", e.r32(MEM + O["pad_id"]) == PAD)

# a hit signal now also plays the pad: the same hits as MIDI, plus the engine commands
dispatched.clear()
out = feed(sig, silence)
want_notes = [m for m in want if m[0] == 0x90]
ons = [(d[1][0], struct.unpack_from("<IIIHH", d[1], 8)) for d in dispatched if d[1][0] == 0x42]
offs = [(d[1][0], struct.unpack_from("<IIIHH", d[1], 8)) for d in dispatched if d[1][0] == 0x43]
check(f"pad target: {len(want_notes)} note-ons dispatched, each the hit's velocity, on the learned pad and note 62",
      [x[1] for x in ons] == [(PAD, 62, 0, 1, m[2]) for m in want_notes], ons[:3])
check("... and a note-off for each (the Dynamic note ending), for the same pad and note",
      len(offs) == len(want_notes) and all(x[1][:2] == (PAD, 62) for x in offs), offs[:3])
check("... all to the engine object the command queue lives in", all(d[0] == OBJ for d in dispatched), [hex(d[0]) for d in dispatched[:2]])
gold = stock(0x0804C61C, PAD, 62, 0, want_notes[0][2])[0][1]
check("the dispatched note-on is byte for byte what the stock builder queues for the same pad, note and velocity",
      [d[1] for d in dispatched if d[1][0] == 0x42][0] == gold, ([d[1] for d in dispatched][0].hex(), gold.hex()))
gold_off = stock(0x0804C5D4, PAD, 62, 0, 0)[0][1]
check("... and the note-off is byte for byte the stock note-off's", [d[1] for d in dispatched if d[1][0] == 0x43][0] == gold_off,
      ([d[1] for d in dispatched if d[1][0] == 0x43][0].hex(), gold_off.hex()))
check("the MIDI out is unchanged by the pad target", msgs(out) == want)

# Note Out OFF: no MIDI note, the hits still count and the pad still plays
e.uc.mem_write(MEM + O["p"] + O["p_note_on"], b"\x00")
dispatched.clear()
h0 = e.r32(MEM + O["hits"])
out = feed(sig, silence)
check("Note Out OFF: no MIDI at all (CC is off too)", out == [], msgs(out)[:3])
check("... the hits are still counted", e.r32(MEM + O["hits"]) - h0 == len(want_notes))
check("... and the pad still gets every note-on", sum(1 for d in dispatched if d[1][0] == 0x42) == len(want_notes))
e.uc.mem_write(MEM + O["p"] + O["p_note_on"], b"\x01")

# fixed velocity applies to the pad as well
e.uc.mem_write(MEM + O["p"] + O["p_vel_mode"], b"\x01")
e.uc.mem_write(MEM + O["p"] + O["p_vel_fixed"], bytes([111]))
dispatched.clear()
feed(sig, silence)
check("VEL fixed 111: the pad gets 111", {struct.unpack_from("<H", d[1], 22)[0] for d in dispatched if d[1][0] == 0x42} == {111})
e.uc.mem_write(MEM + O["p"] + O["p_vel_mode"], b"\x00")

# target off while a note sounds: its note-off goes out, then silence
dispatched.clear()
feed(sig[:900], silence[:900])
e.uc.mem_write(MEM + O["pad_on"], b"\x00")
dispatched.clear()
feed(silence[:N], silence[:N])
check("pad target switched off with a pad note held: one note-off", [d[1][0] for d in dispatched] == [0x43], [d[1][0] for d in dispatched])
dispatched.clear()
feed(sig, silence)
check("pad target off: nothing dispatched", dispatched == [])
e.uc.mem_write(MEM + O["pad_on"], b"\x01")

# the engine not up: no dispatch, no crash
e.w32(ENGINE + 0x9000 + 0xA74, 0)
dispatched.clear()
feed(sig, silence)
check("engine object missing: no dispatch", dispatched == [])
e.w32(ENGINE + 0x9000 + 0xA74, OBJ)

# input switched off with a pad note held
feed(sig[:900], silence[:900])
e.uc.mem_write(MEM + O["on"], b"\x00")
dispatched.clear()
feed(silence[:N], silence[:N])
check("input switched off with a pad note held: the pad note-off goes out", [d[1][0] for d in dispatched] == [0x43])
e.uc.mem_write(MEM + O["on"], b"\x01")

# ---- allocator short at boot: no memory taken, no MIDI, no crash
free_bytes[0] = 100 * 1024
allocs.clear()
e.call("t2m_boot")
check("boot with the allocator short: nothing allocated", allocs == [], allocs)
dispatched.clear()
out = feed(sig[:SR // 2], sig[:SR // 2])
check("... and the inputs stay quiet", out == [] and dispatched == [], out[:2])
e.call("t2m_learn", ENGINE, PAD, 60, 0)
check("... and a stock pad hit with no Trigger2MIDI memory is harmless", True)

print("ALL PASS" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
