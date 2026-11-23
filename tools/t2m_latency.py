"""Trigger2MIDI latency under Unicorn, on the patched image (the real looper_in -> t2m_audio -> detector path).

What this measures: the part of the latency that our code decides. For hits whose first sample lands at every phase of the
256-sample audio block it records, from the real firmware code:
  detect   samples from the threshold crossing to the sample where the detector decides (the scan / onset window),
  queued   from the threshold crossing to the END of the block that holds the decision (the audio task only runs when the
           whole block has been captured, so that is when the MIDI message is queued; the render sends it in the same block),
  wire     3 bytes on the MIDI jack at 31250 baud = 0.96 ms (added for MIDI; a pad target has none),
plus the velocity it read, and how many false triggers a click or noise bed gives at that setting.
What it does NOT measure (needs hardware): the codec's own input latency, how long the audio task takes to start after
the block is captured, the UART's start time, USB scheduling, and the pad voice's output latency.

python3 patch.py solo slice duck chord cond filter cpu od comp seqfix fx2 munchi looper cave && python3 tools/t2m_latency.py
"""
import os
import random
import struct
import subprocess
import sys
from math import exp, pi, sin

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import unicorn.arm_const as A  # noqa: E402
from unicorn import UC_HOOK_CODE  # noqa: E402

from emu import Emu  # noqa: E402

IMG = "out/solo+slice+duck+chord+cond+filter+cpu+od+comp+seqfix+fx2+munchi+looper+cave/BLACKBOX.bin"
SR, N = 48000, 256
MS = SR / 1000.0
WIRE_MS = 3 * 10 / 31250 * 1000
MEM, OBJ, ENGINE = 0x30100000, 0x30050000, 0x2400A9C0
INL, INR, PTRS = 0x30010000, 0x30011000, 0x30012000

fields = ["on", "pad_on", "pad_id", "p", "st", "hits"]
pf = ["sens", "thresh", "retrig", "mask_ms", "scan_ms", "curve", "strict_db", "speed_ms", "spec_strict", "spec_range", "floor_db",
      "note_on", "vel_mode", "vel_fixed"]
os.makedirs("out/t2m_test", exist_ok=True)
open("out/t2m_test/lat_off.c", "w").write(
    '#include <stdio.h>\n#include <stddef.h>\n#include "../../src/t2m_bb.h"\nint main(void){\n'
    + "".join(f'printf("{f} %zu\\n", offsetof(struct t2m_mem, {f}));\n' for f in fields)
    + 'printf("PSIZE %zu\\nev %zu\\nn_ev %zu\\n", sizeof(t2m_params), offsetof(t2m_state, events), offsetof(t2m_state, n_events));\n'
    + 'printf("SSIZE %zu\\n", sizeof(t2m_state));\n'
    + "".join(f'printf("p_{f} %zu\\n", offsetof(t2m_params, {f}));\n' for f in pf) + "}\n")
subprocess.run(["gcc", "-I.", "-o", "out/t2m_test/lat_off", "out/t2m_test/lat_off.c"], check=True)
O = {l.split()[0]: int(l.split()[1]) for l in
     subprocess.run(["out/t2m_test/lat_off"], capture_output=True, text=True, check=True).stdout.splitlines()}

e = Emu(IMG, "out/cave.elf")
e.uc.mem_map(0x38800000, 0x1000)
e.uc.mem_map(0x58024000, 0x1000)
e.uc.mem_map(0xE0001000, 0x1000)
e.uc.mem_map(0xE000E000, 0x1000)                      # DEMCR (the clock monitor switches the cycle counter on)
e.uc.mem_map(0x30000000, 0x200000)
sent, dispatched = [], []
e.stub(0x080443F0, fn=lambda: e.uc.reg_write(A.UC_ARM_REG_R0, 3 << 20))
e.stub(0x08044404, fn=lambda: e.uc.reg_write(A.UC_ARM_REG_R0, MEM))
e.stub(0x08044A0E, fn=lambda: sent.append(bytes(e.uc.mem_read(e.arg(1), 5))))
e.stub(0x080520EC, fn=lambda: dispatched.append(bytes(e.uc.mem_read(e.arg(1), 24))))
e.stub(0x080518F0)
e.w32(PTRS, INL)
e.w32(PTRS + 4, INR)
e.w32(ENGINE + 0x9000 + 0xA74, OBJ)
instr = [0]
e.uc.hook_add(UC_HOOK_CODE, lambda *a: instr.__setitem__(0, instr[0] + 1))


def setup(cfg):
    e.call("t2m_boot")                                   # zeroes the state and sets the patch's defaults
    for k, v in cfg.items():
        if k == "pad":
            e.uc.mem_write(MEM + O["pad_id"], struct.pack("<I", 0x00010203))
            e.uc.mem_write(MEM + O["pad_on"], bytes([1]))
        elif k in ("note_on", "vel_mode", "vel_fixed"):
            e.uc.mem_write(MEM + O["p"] + O["p_" + k], bytes([v]))
        else:
            e.uc.mem_write(MEM + O["p"] + O["p_" + k], struct.pack("<f", v))


def run(signal):
    """Feed the signal in blocks; for each block the note-on frames (from the detector's own event list) and instructions."""
    evs, ins = [], []
    for b in range(0, len(signal), N):
        chunk = signal[b:b + N]
        e.uc.mem_write(INL, struct.pack(f"<{N}f", *(chunk + [0.0] * (N - len(chunk)))))
        e.uc.mem_write(INR, bytes(4 * N))
        sent.clear()
        dispatched.clear()
        instr[0] = 0
        e.call("looper_in", 0x30000000, PTRS, N, count=5_000_000)
        ins.append(instr[0])
        n = e.r32(MEM + O["st"] + O["n_ev"])
        for j in range(n):
            frame, status, d1, d2 = struct.unpack_from("<HBBB", bytes(e.uc.mem_read(MEM + O["st"] + O["ev"] + 6 * j, 6)))
            if status & 0xF0 == 0x90:
                evs.append((b // N, frame, d2, len(sent), len(dispatched)))
    return evs, ins


rng = random.Random(7)


def drum(amp, n=int(SR * 0.12), attack=24):
    hz, ph = rng.uniform(80, 200), rng.uniform(0, 2 * pi)
    return [amp * min(1, (i + 1) / attack) * (0.5 * sin(ph + 2 * pi * hz * i / SR) * exp(-i / (SR * 0.06)) +
                                              0.5 * rng.uniform(-1, 1) * exp(-i / (SR * 0.006))) for i in range(n)]


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def hits_signal(amp, phases, gap=9000, attack=24):
    """One hit per gap, its first sample at phase p of the block (p = 0 .. 255) of block 1+k."""
    sig = [0.0] * (N * (2 + len(phases) * (gap // N + 1)) + 9000)
    starts = []
    for k, p in enumerate(phases):
        t0 = N * (1 + k * (gap // N + 1)) + p
        starts.append(t0)
        for i, x in enumerate(drum(amp, attack=attack)):
            sig[t0 + i] += x
    return [f32(max(-1.0, min(1.0, x))) for x in sig], starts


def measure(cfg, amp, phases):
    setup(cfg)
    sig, starts = hits_signal(amp, phases)
    evs, ins = run(sig)
    sens = cfg.get("sens", 2.0)
    thr = 0.01 + cfg.get("thresh", 2.0) * (0.09 / 31.0)
    rows = []
    for t0 in starts:
        cross = next(i for i in range(t0, len(sig)) if abs(sig[i]) * sens > thr)
        ev = next((x for x in evs if x[0] * N >= t0 - N and x[0] * N + x[1] >= cross and x[0] * N < t0 + 6000), None)
        if ev is None:
            rows.append(None)
            continue
        decide = ev[0] * N + ev[1]
        queued = (ev[0] + 1) * N
        rows.append(((decide - cross) / MS, (queued - cross) / MS, ev[2], ev[3] > 0, ev[4] > 0))
    return rows, ins


def stats(v):
    return min(v), sum(v) / len(v), max(v)


CONFIGS = [
    ("patch defaults: Speed 3.0, Scan 2.0, dynamic velocity", {}),
    ("Speed 1.0, Scan 2.0, dynamic velocity", dict(speed_ms=1.0)),
    ("VEL fixed 100 (Scan 0), Speed 3.0", dict(scan_ms=0.0, vel_mode=1, vel_fixed=100)),
    ("VEL fixed 100 (Scan 0), Speed 1.0", dict(scan_ms=0.0, vel_mode=1, vel_fixed=100, speed_ms=1.0)),
    ("VEL fixed 100 (Scan 0), Speed 0.5", dict(scan_ms=0.0, vel_mode=1, vel_fixed=100, speed_ms=0.5)),
    ("VEL fixed 100 (Scan 0), Speed 0.2", dict(scan_ms=0.0, vel_mode=1, vel_fixed=100, speed_ms=0.2)),
    ("dynamic velocity but Scan 0, Speed 0.2 (not reachable by VEL fixed)", dict(scan_ms=0.0, speed_ms=0.2)),
    ("... Speed 0.2, plus a pad target", dict(scan_ms=0.0, vel_mode=1, vel_fixed=100, speed_ms=0.2, pad=1)),
]
PHASES = list(range(0, 256, 16))


def spikes_and_noise(cfg):
    """False triggers: 12 isolated 3-sample clicks of 0.2 on a quiet bed, and 4 s of a loud noise bed (0.012, above the threshold
    after the gain) that is not a hit."""
    r = random.Random(11)
    sig = [0.002 * r.uniform(-1, 1) for _ in range(SR * 2)]
    for k in range(12):
        t = 3000 + k * 7000
        for i in range(3):
            sig[t + i] += 0.2 * (1 if i != 1 else -1)
    setup(cfg)
    clicks = len(run([f32(x) for x in sig])[0])
    setup(cfg)
    noise = len(run([f32(0.012 * r.uniform(-1, 1)) for _ in range(SR * 4)])[0])
    return clicks, noise


def main():
    global res
    print(f"{'setting':58} {'decide':>13} {'queued (block end)':>21} {'+ MIDI wire':>13} {'velocity':>9} {'false trigs':>12}")
    print(f"{'':58} {'ms min/avg/max':>13} {'ms min/avg/max':>21} {'avg ms':>13} {'loud/soft':>9} {'clicks/noise':>12}")
    res = {}
    for name, cfg in CONFIGS:
        rows, ins = measure(cfg, 0.8, PHASES)
        miss = sum(r is None for r in rows)
        ok = [r for r in rows if r]
        soft = [r for r in measure(cfg, 0.12, PHASES[::4])[0] if r]
        d, q = stats([r[0] for r in ok]), stats([r[1] for r in ok])
        fc = spikes_and_noise(cfg)
        res[name] = (d, q, ok, ins)
        pad = cfg.get("pad")
        wire = "-" if pad else f"{q[1] + WIRE_MS:.1f}"
        vel = f"{round(sum(r[2] for r in ok) / len(ok))}/{round(sum(r[2] for r in soft) / max(1, len(soft)))}"
        print(f"{name:58} {d[0]:4.1f}/{d[1]:4.1f}/{d[2]:4.1f} {q[0]:6.1f}/{q[1]:5.1f}/{q[2]:5.1f} {wire:>13} {vel:>9} {fc[0]:>6}/{fc[1]:<5}"
              + (f"   MISSED {miss}" if miss else ""))
        if pad:
            print(f"{'':58} pad command dispatched in the same block as the decision for {sum(1 for r in ok if r[4])} of {len(ok)} hits"
                  f" (MIDI sent for {sum(1 for r in ok if r[3])})")

    ins = res[CONFIGS[0][0]][3]
    busy = sorted(ins)[-1]
    print(f"\nCPU: looper_in with both inputs on, {sorted(ins)[len(ins) // 2]} instructions in a typical block, {busy} in the busiest; "
          f"at one instruction a cycle and 400 MHz the busiest is {busy / 400:.0f} us of the 5333 us block.")
    print(f"Block wait for a hit at a random phase is 0 .. {N / MS:.2f} ms (avg {N / MS / 2:.2f}); MIDI wire time {WIRE_MS:.2f} ms.")


if __name__ == "__main__":
    main()
