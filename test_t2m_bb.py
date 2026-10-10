"""Trigger2MIDI wired into the firmware (src/t2m_bb.c, called from src/looper.c), under Unicorn on the patched image.

Real code: looper_in (the input stage's tail call) with the looper idle, t2m_boot, t2m_audio, the detector.
Stubbed: the SDRAM allocator and its free-space read, the stock MIDI send FUN_08044a0e (recorded), the input stage tail.
Checked: memory taken once at boot (and not at all when the allocator is short), the MIDI messages of a hit signal on
input L are the reference's (t2m_ref.py) and go to the stock port manager with the right TRS / USB flags, input R is
silent while off and follows its own port when on, switching an input off ends its sounding note.

python3 patch.py solo slice duck chord cond filter cpu od comp seqfix fx2 munchi looper cave && python3 test_t2m_bb.py
"""
import os
import struct
import subprocess
import sys

import t2m_ref as ref
from emu import Emu

IMG = "out/solo+slice+duck+chord+cond+filter+cpu+od+comp+seqfix+fx2+munchi+looper+cave/BLACKBOX.bin"
SR, N = 48000, 256
MGR = 0x24002E88
ROOT = 0x38800980
MEM = 0x30100000
INL, INR, PTRS = 0x30010000, 0x30011000, 0x30012000

fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


# struct t2m_mem / t2m_params offsets, from the header itself (same layout on the host: only 1-, 2- and 4-byte fields)
os.makedirs("out/t2m_test", exist_ok=True)
open("out/t2m_test/off.c", "w").write(
    '#include <stdio.h>\n#include <stddef.h>\n#include "../../src/t2m_bb.h"\nint main(void){printf("%zu %zu %zu %zu %zu %zu '
    '%zu %zu %zu %zu\\n", sizeof(struct t2m_mem), offsetof(struct t2m_mem, on), offsetof(struct t2m_mem, port), '
    'offsetof(struct t2m_mem, p), sizeof(t2m_params), offsetof(struct t2m_mem, hits), offsetof(struct t2m_mem, st), '
    'offsetof(t2m_params, note_on), offsetof(t2m_params, len_mode), offsetof(struct t2m_mem, last_vel));}\n')
subprocess.run(["gcc", "-o", "out/t2m_test/off", "out/t2m_test/off.c"], check=True)
SIZE, O_ON, O_PORT, O_P, PSIZE, O_HITS, O_ST, OP_NOTE_ON, OP_LEN_MODE, O_LASTV = map(
    int, subprocess.run(["out/t2m_test/off"], capture_output=True, text=True, check=True).stdout.split())

e = Emu(IMG, "out/cave.elf")
e.uc.mem_map(0x38800000, 0x1000)                     # backup SRAM
e.uc.mem_map(0x58024000, 0x1000)                     # RCC / PWR
e.uc.mem_map(0xE0001000, 0x1000)                     # DWT
e.uc.mem_map(0x30000000, 0x200000)                   # buffers + the "SDRAM" block the allocator hands out

free_bytes = [3 << 20]
allocs, sent = [], []
e.stub(0x080443F0, value=None, fn=lambda: e.uc.reg_write(__import__("unicorn").arm_const.UC_ARM_REG_R0, free_bytes[0]))


def alloc():
    allocs.append(e.arg(0))
    e.uc.reg_write(__import__("unicorn").arm_const.UC_ARM_REG_R0, MEM)


e.stub(0x08044404, fn=alloc)
e.stub(0x08044A0E, fn=lambda: sent.append((e.arg(0), bytes(e.uc.mem_read(e.arg(1), 5)), e.arg(2), e.arg(3))))
e.stub(0x080518F0)                                    # the input stage's own tail (FUN_080518f0)
e.w32(PTRS, INL)
e.w32(PTRS + 4, INR)


def feed(left, right):
    """Blocks through looper_in (the real hook), as the audio task does."""
    sent.clear()
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


# ---- boot
e.call("t2m_boot")
check(f"boot: one allocation of the settings + two detectors ({SIZE} bytes)", allocs == [SIZE], allocs)
check("boot: the root in the backup SRAM points at it", e.r32(ROOT) == 0x54324D31 and e.r32(ROOT + 4) == MEM,
      (hex(e.r32(ROOT)), hex(e.r32(ROOT + 4))))
check("boot: input L on, R off, both on TRS + USB",
      [e.r8(MEM + O_ON), e.r8(MEM + O_ON + 1), e.r8(MEM + O_PORT), e.r8(MEM + O_PORT + 1)] == [1, 0, 3, 3])

# a hit signal: 16ths with accents (the same generator as test_t2m.py, shortened)
import random
from math import exp, pi, sin

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
hits = e.r32(MEM + O_HITS)
check(f"page counters: {hits} hits, last value {e.r8(MEM + O_LASTV)}", hits == sum(1 for m in want if m[0] == 0x90))

out = feed(silence, sig)
check("input R off: nothing sent for hits on R", out == [], out[:3])

# input R on, USB only, its own note
e.uc.mem_write(MEM + O_ON + 1, b"\x01")
e.uc.mem_write(MEM + O_PORT + 1, b"\x02")
e.uc.mem_write(MEM + O_P + PSIZE + OP_NOTE_ON + 1, bytes([45]))    # note (follows note_on in the struct)
out = feed(silence, sig)
check("input R on: the same hits, on its own pitch (45)", [m[1] for m in msgs(out)] == [45 if m[1] == 38 else m[1] for m in want],
      msgs(out)[:4])
check("input R on USB only: uart flag 0, usb flag set", out and all(not uart and usb for _, (_, _, uart, usb) in out), out[:2])

# both inputs at once: L's and R's messages interleave per block, nothing lost
out = feed(sig, sig)
check("both inputs: twice the messages", len(out) == 2 * len(want), (len(out), len(want)))

# switching L off while its Dynamic note is held ends that note
held = sig[:6000 + 300] + [0.0] * 0
out = feed(sig[:900], silence[:900])                 # a hit, note still sounding (Dynamic length)
on_notes = [m for m in msgs(out) if m[0] == 0x90]
e.uc.mem_write(MEM + O_ON, b"\x00")
out = feed(silence[:N], silence[:N])
check("input L switched off with a note held: its note-off goes out", on_notes and msgs(out) == [(0x80, 38, 0)], msgs(out))
out = feed(sig, silence)
check("input L off: silent", out == [])
e.uc.mem_write(MEM + O_ON, b"\x01")

# allocator short at boot: no memory taken, no MIDI, no crash
free_bytes[0] = 100 * 1024
allocs.clear()
e.call("t2m_boot")
check("boot with the allocator short: nothing allocated", allocs == [], allocs)
out = feed(sig[:SR // 2], sig[:SR // 2])
check("... and the inputs stay quiet", out == [], out[:2])

print("ALL PASS" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
