"""Trigger2MIDI wired into the firmware (src/t2m_bb.c, src/t2m_thunk.S, called from src/looper.c), under Unicorn on the
patched image.

Real code: looper_in (the input stage's tail call) with the looper idle, t2m_boot, t2m_audio, the detector.
Stubbed: the SDRAM allocator and its free-space read, the stock MIDI send FUN_08044a0e (recorded), the input stage tail.
Checked: memory taken once at boot (and not at all when the allocator is short), the MIDI messages of a hit signal on
input L are the reference's (t2m_ref.py) and go to the stock port manager with the right TRS / USB flags, input R is
silent while off and follows its own port when on, switching an input off ends its sounding note; INT (the Blackbox's own
MIDI in): the same messages land in the stock software incoming ring (entries, write index, full ring, wrap), alone or
together with the jack, and Note Out OFF keeps the hits but sends no note.

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
RING = 0x24002E88 + 0x4000                            # the stock software incoming ring: entries +0x660 + 5 n, read +0xb60, write +0xb64
MAGIC = 0x54324D32

fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


# struct offsets from the header itself (same layout on the host: only 1-, 2- and 4-byte fields)
FIELDS = ["on", "port", "last_vel", "last_note", "hits", "peak", "blocks", "hit_blk", "looped", "looped_lost", "hist_n", "hist",
          "ui", "p", "st"]
os.makedirs("out/t2m_test", exist_ok=True)
open("out/t2m_test/off.c", "w").write(
    '#include <stdio.h>\n#include <stddef.h>\n#include "../../src/t2m_bb.h"\nint main(void){\n'
    + "".join(f'printf("{f} %zu\\n", offsetof(struct t2m_mem, {f}));\n' for f in FIELDS)
    + 'printf("SIZE %zu\\nPSIZE %zu\\nHIT %zu\\n", sizeof(struct t2m_mem), sizeof(t2m_params), sizeof(struct t2m_hit));\n'
    + "".join(f'printf("p_{f} %zu\\n", offsetof(t2m_params, {f}));\n' for f in ("note", "note_on", "vel_mode", "vel_fixed", "cc_on", "cc_num"))
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
allocs, sent = [], []
e.stub(0x080443F0, fn=lambda: e.uc.reg_write(A.UC_ARM_REG_R0, free_bytes[0]))


def alloc():
    allocs.append(e.arg(0))
    e.uc.reg_write(A.UC_ARM_REG_R0, MEM)


e.stub(0x08044404, fn=alloc)
e.stub(0x08044A0E, fn=lambda: sent.append((e.arg(0), bytes(e.uc.mem_read(e.arg(1), 5)), e.arg(2), e.arg(3))))
e.stub(0x080518F0)                                    # the input stage's own tail (FUN_080518f0)
e.w32(PTRS, INL)
e.w32(PTRS + 4, INR)


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


def ring():
    """The entries the Blackbox's own MIDI in has not read yet: (status, d1, d2, spare, length), and the two indexes."""
    rd, wr = e.r32(RING + 0xB60) & 0xFFFFFFFF, e.r32(RING + 0xB64) & 0xFFFFFFFF
    ents = [tuple(e.uc.mem_read(RING + 0x660 + (n & 0xFF) * 5, 5)) for n in range(rd, wr)]
    return ents, rd, wr


def set_ring(rd, wr):
    e.w32(RING + 0xB60, rd)
    e.w32(RING + 0xB64, wr)


# ---- boot
e.call("t2m_boot")
check(f"boot: one allocation of the settings + two detectors ({SIZE} bytes)", allocs == [SIZE], allocs)
check("boot: the root in the backup SRAM points at it", e.r32(ROOT) == MAGIC and e.r32(ROOT + 4) == MEM,
      (hex(e.r32(ROOT)), hex(e.r32(ROOT + 4))))
check("boot: input L on, R off, both on TRS + USB, not into the Blackbox itself (INT off)",
      [e.r8(MEM + O["on"]), e.r8(MEM + O["on"] + 1), e.r8(MEM + O["port"]), e.r8(MEM + O["port"] + 1)] == [1, 0, 3, 3])

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
check("INT off: the Blackbox's own incoming ring is untouched", ring()[0] == [] and ring()[2] == 0)

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

# ---- INT: the Blackbox's own MIDI in
P_ALL = 7
e.uc.mem_write(MEM + O["port"], bytes([P_ALL]))
set_ring(0, 0)
out = feed(sig, silence)
ents, rd, wr = ring()
want5 = [(st, d1, d2, 0, 3) for st, d1, d2 in want]
check(f"INT + jack + USB: the same {len(want)} messages are in the Blackbox's incoming ring, in order, as five-byte entries", ents == want5, (ents[:3], want5[:3]))
check("... the write index moved once per message and the read index not at all", wr == len(want) and rd == 0, (rd, wr))
check("... the MIDI out is unchanged by INT", msgs(out) == want)
check(f"... the page counter: {e.r32(MEM + O['looped'])} sent, none lost", e.r32(MEM + O["looped"]) == len(want) and e.r32(MEM + O["looped_lost"]) == 0)

e.uc.mem_write(MEM + O["port"], bytes([4]))                   # INT only
set_ring(0, 0)
out = feed(sig, silence)
check("INT only: nothing goes to the jack or USB", out == [], msgs(out)[:3])
check("... all of it into the Blackbox", ring()[0] == want5)

set_ring(250, 250)
feed(sig, silence)
ents, rd, wr = ring()
check("the ring index wraps at 256 entries: the entries after #255 land at 0, 1, ...", ents == want5 and wr == 250 + len(want), (rd, wr))
check("... the slots really are (n & 255) * 5 apart",
      bytes(e.uc.mem_read(RING + 0x660 + 250 * 5, 5)) == bytes(want5[0]))

set_ring(0, 255)
before = e.r32(MEM + O["looped_lost"])
feed(sig, silence)
check("ring full (255 unread): nothing is added, the index stays, the lost counter counts",
      ring()[2] == 255 and e.r32(MEM + O["looped_lost"]) - before == len(want), (ring()[2], e.r32(MEM + O["looped_lost"]) - before))
set_ring(0, 0)

# Note Out OFF: no note into the Blackbox either (the hits still count)
e.uc.mem_write(MEM + O["p"] + O["p_note_on"], b"\x00")
h0 = e.r32(MEM + O["hits"])
feed(sig, silence)
check("Note Out OFF: no note in the ring", ring()[0] == [], ring()[0][:2])
check("... but the hits are still counted", e.r32(MEM + O["hits"]) - h0 == len(want_notes := [m for m in want if m[0] == 0x90]))
# CC on: a CC per hit goes in even with Note Out off
e.uc.mem_write(MEM + O["p"] + O["p_cc_on"], b"\x01")
e.uc.mem_write(MEM + O["p"] + O["p_cc_num"], bytes([74]))
feed(sig, silence)
ents = ring()[0]
check("CC on, Note Out off: one CC per hit (74, channel 1) in the ring and nothing else",
      len(ents) == len(want_notes) and all(x[0] == 0xB0 and x[1] == 74 for x in ents), ents[:3])
e.uc.mem_write(MEM + O["p"] + O["p_cc_on"], b"\x00")
e.uc.mem_write(MEM + O["p"] + O["p_note_on"], b"\x01")
set_ring(0, 0)

# switching the input off while a Dynamic note is held: the note-off goes into the ring too
feed(sig[:900], silence[:900])
e.uc.mem_write(MEM + O["on"], b"\x00")
n0 = ring()[2]
feed(silence[:N], silence[:N])
check("input switched off with a note held: its note-off goes into the ring", ring()[0][-1] == (0x80, 38, 0, 0, 3) and ring()[2] == n0 + 1, ring()[0][-2:])
e.uc.mem_write(MEM + O["on"], b"\x01")
e.uc.mem_write(MEM + O["port"], bytes([3]))
set_ring(0, 0)

# ---- allocator short at boot: no memory taken, no MIDI, no crash
free_bytes[0] = 100 * 1024
allocs.clear()
e.call("t2m_boot")
check("boot with the allocator short: nothing allocated", allocs == [], allocs)
out = feed(sig[:SR // 2], sig[:SR // 2])
check("... and the inputs stay quiet", out == [] and ring()[0] == [], out[:2])

print("ALL PASS" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
