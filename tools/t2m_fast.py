"""Trigger2MIDI with click-like hits and very fast trains, under Unicorn on the patched image (the real looper_in path, via
tools/t2m_latency.py's harness), and WHY a hit is dropped: the literal gen~ transcription (t2m_ref.Gen) is stepped over the
same signal and asked, at each missed hit's threshold crossing, which gate was shut.

  A. clicks: a 1-sample impulse, a 3-sample tick, a 2 ms 1.5 kHz click, a 4 kHz wood click (10 ms ring): detection rate over
     all phases of the audio block, and the time from the click's first sample to the queued MIDI.
  B. trains of 8 equal hits at shrinking intervals: how many are found, how many extra, and the gate that stopped each miss.

python3 patch.py solo slice duck chord cond filter cpu od comp seqfix fx2 munchi looper cave && python3 tools/t2m_fast.py
"""
import os
import random
import sys
from math import exp, pi, sin

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import t2m_latency as L  # noqa: E402  (builds the emulator and the harness)

import t2m_ref as ref  # noqa: E402

SR, N, MS = L.SR, L.N, L.MS


def tone(hz, tau_ms, n, amp, attack=0):
    return [amp * (min(1.0, (i + 1) / attack) if attack else 1.0) * sin(2 * pi * hz * i / SR) * exp(-i / (tau_ms * MS))
            for i in range(n)]


SHAPES = {
    "1-sample impulse": lambda amp: [amp],
    "3-sample tick": lambda amp: [amp, -amp, amp],
    "2 ms click, 1.5 kHz": lambda amp: tone(1500, 0.5, int(2 * MS), amp),
    "wood click, 4 kHz, 10 ms ring": lambda amp: tone(4000, 2.0, int(10 * MS), amp),
    "snare-like (noise, 25 ms)": lambda amp: [amp * random.Random(i).uniform(-1, 1) * exp(-i / (6 * MS)) for i in range(int(25 * MS))],
}

CONFIGS = [
    ("defaults: Speed 3.0, Scan 2.0, Mask 15, Retrig 8", {}),
    ("Speed 1.0, Scan 2.0", dict(speed_ms=1.0)),
    ("VEL fixed (Scan 0), Speed 0.5", dict(scan_ms=0.0, vel_mode=1, vel_fixed=100, speed_ms=0.5)),
    ("VEL fixed (Scan 0), Speed 0.2", dict(scan_ms=0.0, vel_mode=1, vel_fixed=100, speed_ms=0.2)),
    ("+ FLOOR -100 dB (Speed 0.5)", dict(scan_ms=0.0, vel_mode=1, vel_fixed=100, speed_ms=0.5, floor_db=-100.0, mask_ms=5.0)),
]


def f32(x):
    return L.f32(max(-1.0, min(1.0, x)))


# ---------------------------------------------------------------- A. clicks
def clicks(cfg, shape, amp):
    L.setup(cfg)
    phases = list(range(0, 256, 16))
    gap = 6144
    sig = [0.0] * (N * (2 + len(phases) * 24) + 4000)
    starts = []
    for k, p in enumerate(phases):
        t0 = N * (1 + k * 24) + p
        starts.append(t0)
        for i, x in enumerate(shape(amp)):
            sig[t0 + i] += x
    sig = [f32(x) for x in sig]
    evs, _ = L.run(sig)
    out = []
    for t0 in starts:
        ev = next((x for x in evs if t0 - N <= x[0] * N < t0 + 3000 and x[0] * N + x[1] >= t0), None)
        out.append(None if ev is None else ((ev[0] + 1) * N - t0) / MS)
    return out, len(evs) - sum(o is not None for o in out)


print("A. CLICKS: detected of 16 block phases / extra triggers / ms from the first sample to the queued MIDI (min avg max); + 0.96 ms wire\n")
print(f"{'':32}" + "".join(f"{c[0][:36]:>38}" for c in CONFIGS))
for sname, shape in SHAPES.items():
    for amp in (0.6, 0.1):
        row = f"{sname + f' @{amp}':32}"
        for cname, cfg in CONFIGS:
            ms, extra = clicks(cfg, shape, amp)
            got = [m for m in ms if m is not None]
            row += (f"{len(got):>4}/16 +{extra} " + (f"{min(got):4.1f} {sum(got) / len(got):4.1f} {max(got):4.1f}" if got else "      -        ")).rjust(38)
        print(row)

if "--clicks" in sys.argv:
    sys.exit(0)

# ---------------------------------------------------------------- B. fast trains
def train(shape, amp, interval_ms, count=8, lead=3000):
    t = [lead + round(k * interval_ms * MS) for k in range(count)]
    sig = [0.0] * (t[-1] + int(0.3 * SR))
    for t0 in t:
        for i, x in enumerate(shape(amp)):
            sig[t0 + i] += x
    return [f32(x) for x in sig], t


def why(sig, cfg, t0):
    """Step the gen~ transcription over the signal; at the threshold crossing at / after t0 report the closed gate."""
    p = dict(ref.DEFAULTS)
    p.update({k: v for k, v in cfg.items() if k in p})
    thr = 0.01 + (p["thresh"] / 31.0) * 0.09
    g = ref.Gen()
    for i, x in enumerate(sig[:t0 + 4000]):
        before = (g.state, g.has_dipped, g.dynamic_thresh, g.timer)
        o = g.step(x * p["sens"], thr, p["scan_ms"], p["mask_ms"], p["retrig"], p["curve"], 0.0, p["strict_db"],
                   p["speed_ms"], p["spec_strict"], p["spec_range"])
        if i >= t0 and abs(x) * p["sens"] > thr:
            st, dipped, dyn, tm = before
            if st == 3:
                return f"mask (state 3, {tm / MS:.0f} ms of {p['mask_ms']:.0f} ms)"
            if st in (1, 2):
                return f"inside the previous hit's {p['scan_ms'] if p['scan_ms'] * MS > 3 * p['speed_ms'] * MS else 3 * p['speed_ms']:.1f} ms scan window"
            if not dipped:
                return "not re-armed (has_dipped = 0: the level never fell below half the retrigger threshold)"
            if abs(x) * p["sens"] <= dyn:
                return f"under the retrigger threshold ({dyn:.3f}, decaying with Retrig {p['retrig']:.0f})"
            return "onset check / spectral check (strictness) rejected it"
    return "never crossed the threshold"


INTERVALS = [40, 25, 20, 16, 12, 10, 8, 6, 5, 4, 3]
if "--clicks" in sys.argv:
    sys.exit(0)
TRAIN_CONFIGS = CONFIGS[:4] + [
    ("Speed 0.5, Scan 0, Mask 5 (range opened up)", dict(scan_ms=0.0, vel_mode=1, vel_fixed=100, speed_ms=0.5, mask_ms=5.0)),
    ("Speed 0.5, Scan 0, Mask 5, Retrig 1", dict(scan_ms=0.0, vel_mode=1, vel_fixed=100, speed_ms=0.5, mask_ms=5.0, retrig=1.0)),
    ("Speed 0.5, Scan 0, Mask 2, Retrig 1, FLOOR -100", dict(scan_ms=0.0, vel_mode=1, vel_fixed=100, speed_ms=0.5, mask_ms=2.0, retrig=1.0, floor_db=-100.0)),
]
print("\nB. FAST TRAINS: 8 equal hits at the interval (ms) in the header; found / 8 (+ extra triggers)\n")
for shname in ("2 ms click, 1.5 kHz", "snare-like (noise, 25 ms)"):
    shape = SHAPES[shname]
    print(f"--- {shname} @0.6")
    print(f"{'':50}" + "".join(f"{i:>7}" for i in INTERVALS))
    detail = {}
    for cname, cfg in TRAIN_CONFIGS:
        row = f"{cname:50}"
        for iv in INTERVALS:
            sig, t = train(shape, 0.6, iv)
            L.setup(cfg)
            evs, _ = L.run(sig)
            hit_t = [e[0] * N + e[1] for e in evs]
            found = 0
            used = set()
            for t0 in t:
                m = next((j for j, h in enumerate(hit_t) if j not in used and t0 <= h < t0 + int(max(iv, 6) * MS)), None)
                if m is not None:
                    used.add(m)
                    found += 1
            extra = len(hit_t) - found
            row += f"{found:>5}/8" + (f"+{extra}" if extra else "  ")
            if found < 8 and iv in (25, 12) and shname.startswith("snare"):
                miss = next(t0 for t0 in t if not any(t0 <= h < t0 + int(max(iv, 6) * MS) for h in hit_t))
                detail[(cname, iv)] = why(sig, cfg, miss)
        print(row)
    for (cname, iv), w in detail.items():
        print(f"    {cname[:44]:44} at {iv} ms, the first miss: {w}")
    print()
