"""LIRA-8 engine (src/lira.c) on the host: compiled with the system gcc and driven through ctypes.

Checks the engine against what is documented about the hardware (docs/LIRA-analog-notes.md): oscillator pitch and
waveform, aliasing, envelope times, the Hold thresholds, bleed, FM sensitivity falling with pitch, vibrato, the delay's
time law and its two memory regimes, the shared feedback loop; plus robustness under random settings and CPU cost.
NOT checked: how it sounds against the real instrument (no recording of one here), or anything on the device.
"""
import ctypes
import math
import os
import random
import subprocess
import sys
import tempfile
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
tmp = tempfile.mkdtemp()
so = os.path.join(tmp, "lira.so")
subprocess.check_call(["gcc", "-O2", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror", "-o", so,
                       os.path.join(HERE, "src", "lira.c")])
lib = ctypes.CDLL(so)
lib.lira_size.restype = ctypes.c_int
lib.lira_init.argtypes = [ctypes.c_void_p, ctypes.c_float, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_uint32]
lib.lira_param.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_float]
lib.lira_key.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
lib.lira_touch.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_float]
lib.lira_process.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]

# parameter ids, same order as the enum in lira.h
NAMES = ("TUNE1 TUNE2 TUNE3 TUNE4 TUNE5 TUNE6 TUNE7 TUNE8 FAST12 FAST34 FAST56 FAST78 SHARP12 SHARP34 SHARP56 SHARP78 "
         "MOD12 MOD34 MOD56 MOD78 SOURCE12 SOURCE34 SOURCE56 SOURCE78 PITCH1234 PITCH5678 HOLD1234 HOLD5678 SWITCH "
         "TOTAL_FB VIBRATO DRV DST_MIX VOL F_A F_B ANDOR LINK TIME1 TIME2 FEEDBACK DEL_MIX MOD1 MOD2 DEL_MOD LFO_WAV "
         "QUANTIZE").split()
P = {n: i for i, n in enumerate(NAMES)}
assert len(NAMES) == 47

SR = 44100
DL = 44032


class Lira:
    def __init__(self, seed=1, **params):
        self.mem = ctypes.create_string_buffer(lib.lira_size())
        self.d1 = (ctypes.c_float * DL)()
        self.d2 = (ctypes.c_float * DL)()
        lib.lira_init(self.mem, SR, self.d1, self.d2, DL, seed)
        for k, v in params.items():
            self.set(k, v)

    def set(self, name, v):
        lib.lira_param(self.mem, P[name], v)

    def key(self, i, on):
        lib.lira_key(self.mem, i, on)

    def touch(self, i, d):
        lib.lira_touch(self.mem, i, d)

    def run(self, n):
        out = (ctypes.c_float * n)()
        lib.lira_process(self.mem, out, n)
        return np.frombuffer(out, dtype=np.float32).astype(np.float64)


# a bare voice: no delay, no distortion, no FM, no vibrato, only osc 1 (index 0) can sound
CLEAN = dict(DEL_MIX=0, DST_MIX=0, VIBRATO=0, FAST12=1, SHARP12=0, MOD12=0, SOURCE12=1, PITCH1234=127, TUNE1=100)


def mtof(m):
    return 440.0 * 2 ** ((m - 69) / 12)


def spectrum(x, n=1 << 17):
    w = x[:n] * np.hanning(len(x[:n]))
    return np.abs(np.fft.rfft(w, n)), np.fft.rfftfreq(n, 1 / SR)


def peak_hz(x, lo=20, hi=8000):
    s, f = spectrum(x)
    m = (f > lo) & (f < hi)
    return f[m][np.argmax(s[m])]


def rms(x):
    return float(np.sqrt(np.mean(x * x)))


def db(x):
    return 20 * math.log10(max(x, 1e-12))


def periods(x):
    """zero-crossing intervals (samples) of the rising edges, after removing the mean"""
    x = x - np.mean(x)
    idx = np.where((x[:-1] < 0) & (x[1:] >= 0))[0]
    frac = -x[idx] / (x[idx + 1] - x[idx])
    t = idx + frac
    return np.diff(t)


def onset(x, win=441, frac=0.5):
    """time (s) at which the 10 ms rms first reaches `frac` of its final value"""
    n = len(x) // win
    r = np.sqrt(np.mean(x[:n * win].reshape(n, win) ** 2, axis=1))
    ref = np.max(r[-max(n // 10, 1):])
    k = int(np.argmax(r >= frac * ref))
    return k * win / SR


fails = 0


def check(name, ok, info=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + ("  " + info if info else ""))
    fails += not ok


# ---------------------------------------------------------------- voice: pitch, waveform, aliasing
l = Lira(**CLEAN)
l.key(0, 1)
l.run(SR)
y = l.run(SR * 2)
want = mtof(-16 + 100 / 127 * (93 + 16)) * (127 / 127 * 1.99 + 0.01)
got = peak_hz(y)
check("osc 1 pitch", abs(got / want - 1) < 0.005, "want %.1f Hz got %.1f Hz" % (want, got))
check("osc 1 audible, bounded", 0.01 < np.max(np.abs(y)) <= 1.0, "peak %.3f" % np.max(np.abs(y)))
s, f = spectrum(y)
h = [s[np.argmin(np.abs(f - want * k))] for k in range(1, 8)]
check("triangle off the timing capacitor: odd harmonics present but weak (the exponential segments round it), even ones low",
      0.015 < h[2] / h[0] < 0.2 and h[1] / h[0] < 0.05 and h[3] / h[0] < 0.05,
      "h2 %.3f h3 %.3f h4 %.3f" % (h[1] / h[0], h[2] / h[0], h[3] / h[0]))

# aliasing of the square (sharp = 1) at a high pitch: everything that is not a harmonic must be far below the fundamental
l = Lira(**{**CLEAN, "SHARP12": 127, "TUNE1": 118})
l.key(0, 1)
l.run(SR)
y = l.run(SR * 3)
f0 = peak_hz(y, 100, 12000)
s, f = spectrum(y)
harm = np.zeros(len(f), bool)
for k in range(1, int(22000 / f0) + 1):
    harm |= np.abs(f - k * f0) < 0.012 * k * f0 + 3.5          # the oscillator jitters a little (supply coupling)
inband = (f > 100) & (f < 20000)
pw_f = np.sum(s[np.abs(f - f0) < 0.012 * f0 + 3.5] ** 2)
pw_alias = np.sum(s[inband & ~harm] ** 2)
alias_db = 10 * math.log10(pw_f / pw_alias)
check("square at %.0f Hz: non-harmonic content %.0f dB below the fundamental (an uncorrected square gives 13 dB)" % (f0, alias_db),
      alias_db > 24)

# units differ (tolerances), pitch does not
ys = []
for seed in (1, 2, 3):
    l = Lira(seed=seed, **CLEAN)
    l.key(0, 1)
    l.run(SR)
    ys.append(l.run(SR))
pk = [peak_hz(v) for v in ys]
dif = rms(ys[0] - ys[1]) / rms(ys[0])
check("units differ in waveform (threshold tolerance) but not in pitch", max(pk) / min(pk) < 1.005 and dif > 0.01,
      "peaks %s, waveform difference %.3f" % (["%.1f" % p for p in pk], dif))

# ---------------------------------------------------------------- envelope
l = Lira(**CLEAN)
full = None
l.key(0, 1)
l.run(SR)
full = rms(l.run(SR // 4))
l = Lira(**CLEAN)
l.touch(0, 0.4)
l.run(SR)
part = rms(l.run(SR // 4))
check("a light touch opens the envelope partly", 0.05 < part / full < 0.3, "%.1f dB" % db(part / full))

# -60 dB in ~0.5 s (FAST) / ~23 s (slow, tau 6.6 s = 3 Mohm x 2.2 uF; SoS ~20 s, Waveform ~30 s) means -40 dB in 2/3 of that
for fast, lo, hi, what in ((1, 0.2, 0.6, "FAST: about 0.35 s"), (0, 11.0, 20.0, "slow: ~15 s")):
    ts = []
    for seed in (1, 2, 3):
        l = Lira(seed=seed, **{**CLEAN, "FAST12": fast})
        l.key(0, 1)
        l.run(SR)
        ref = rms(l.run(SR // 4))
        l.key(0, 0)
        t, thr = 0.0, ref * 0.01                      # -40 dB: above the bleed floor
        while t < 40:
            if rms(l.run(SR // 20)) < thr:
                break
            t += 0.05
        ts.append(t)
    check("release to -40 dB, %s (seeds: %s s)" % (what, ", ".join("%.2f" % v for v in ts)),
          all(lo <= v <= hi for v in ts))

# hold: slow voices drone from ~12 o'clock, FAST voices from ~2 o'clock
def opened(fast):
    """the four voices of group 1234 (osc 1..4) fully open, as Hold opens them"""
    l = Lira(**{**CLEAN, "FAST12": fast, "FAST34": fast})
    for k in range(4):
        l.key(k, 1)
    l.run(SR)
    return rms(l.run(SR // 4))


def held(fast, hold):
    l = Lira(**{**CLEAN, "FAST12": fast, "FAST34": fast, "HOLD1234": hold * 127})
    l.run(SR // 2)
    return rms(l.run(SR // 4))


full_slow, full_fast = opened(0), opened(1)
l = Lira(**CLEAN)
l.key(0, 1)
l.run(SR)
full = rms(l.run(SR // 4))
h_slow = [db(held(0, h) / full_slow) for h in (0.4, 0.6, 1.0)]
h_fast = [db(held(1, h) / full_fast) for h in (0.6, 0.8, 1.0)]
check("hold, slow voice: silent at 0.4, droning at 0.6 (12 o'clock threshold), fully open at the top", h_slow[0] < -45 and h_slow[1] > -30 and abs(h_slow[2]) < 1.5,
      "%s dB" % ["%.0f" % v for v in h_slow])
check("hold, FAST voice: silent at 0.6, droning at 0.8 (2 o'clock threshold), fully open at the top", h_fast[0] < -45 and h_fast[1] > -30 and abs(h_fast[2]) < 1.5,
      "%s dB" % ["%.0f" % v for v in h_fast])

# ---------------------------------------------------------------- bleed (no output VCA)
bleed = rms(Lira(DEL_MIX=0, DST_MIX=0, FAST12=1).run(SR))
check("nothing touched: a low-level bleed remains (-75 .. -40 dB re one open voice)", -75 < db(bleed / full) < -40,
      "%.0f dB" % db(bleed / full))
hi_t = {f"TUNE{i}": 120 for i in range(1, 9)}
lo_t = {f"TUNE{i}": 20 for i in range(1, 9)}
b_hi = rms(Lira(DEL_MIX=0, DST_MIX=0, **hi_t).run(SR))
b_lo = rms(Lira(DEL_MIX=0, DST_MIX=0, **lo_t).run(SR))
check("bleed rises with tuning (capacitive coupling)", b_hi > 1.5 * b_lo, "%.0f dB vs %.0f dB" % (db(b_hi), db(b_lo)))

# ---------------------------------------------------------------- vibrato
def jitter(vib, seed=1):
    l = Lira(seed=seed, **{**CLEAN, "VIBRATO": vib})
    l.key(0, 1)
    l.run(SR)
    y = l.run(SR * 8)
    p = periods(y)
    k = max(int(0.1 * SR / np.mean(p)), 1)                  # 0.1 s average: the vibrato (0.45-2.6 Hz) survives, jitter does not
    ps = np.convolve(p, np.ones(k) / k, mode="valid")
    return float(np.std(ps) / np.mean(ps))


j_off, j_on = jitter(0), jitter(1)
check("vibrato: each voice drifts a few cents when on, little when off (smoothed period variation %.4f on, %.5f off)" % (j_on, j_off),
      j_on > 0.002 and j_on > 4 * j_off)

# ---------------------------------------------------------------- FM: higher voices are less sensitive
def fm_spread(tune):
    """the LFO (a three-level square) modulates osc 1 alone: relative spread of its frequency, p5 .. p95 of the periods"""
    l = Lira(**{**CLEAN, "TUNE1": tune, "SOURCE12": 2, "MOD12": 60, "F_A": 100, "F_B": 110, "FAST12": 1})
    l.key(0, 1)
    l.run(SR)
    y = l.run(SR * 12)
    p = periods(y)
    lo, hi = np.percentile(p, [5, 95])
    return float((hi - lo) / np.median(p)), float(SR / np.median(p))


(dev_lo, f_lo), (dev_hi, f_hi) = fm_spread(40), fm_spread(112)
check("FM: the higher the voice is tuned, the less it is modulated (relative spread %.3f at %.0f Hz vs %.3f at %.0f Hz)" % (dev_lo, f_lo, dev_hi, f_hi),
      dev_lo > 0.01 and dev_hi < 0.2 * dev_lo)

# ---------------------------------------------------------------- delay (PT2399 model)
def td_of(knob):
    return 0.031 * 2 ** (7.5826 * knob / 127)


def delay_lag(knob, secs):
    kw = {**CLEAN, "TIME1": knob, "TIME2": knob, "FEEDBACK": 0, "MOD1": 0, "MOD2": 0}
    dry = Lira(**{**kw, "DEL_MIX": 0})
    wet = Lira(**{**kw, "DEL_MIX": 127, "DRV": 0})
    for l in (dry, wet):
        l.touch(0, 0.5)                                  # a slow, partial attack gives a clear onset
    yd, yw = dry.run(SR * secs), wet.run(SR * secs)
    return onset(yw) - onset(yd)


for knob, secs in ((45, 3), (95, 8)):
    td = td_of(knob)
    lag = delay_lag(knob, secs)
    regime = "tap (clock above the audio rate)" if td < 44000 / SR else "stepped memory with the 1-bit codec"
    check("delay time law: %s at knob %d is %.2f s, measured %.2f s" % (regime, knob, td, lag), abs(lag - td) < 0.12 * td + 0.05)

# noise: most audible at the longest times
def idle(knob):
    l = Lira(DEL_MIX=127, DST_MIX=0, TIME1=knob, TIME2=knob, FEEDBACK=0, DRV=0, FAST12=1, FAST34=1, FAST56=1, FAST78=1)
    l.run(SR * (2 if knob < 80 else 8))
    return rms(l.run(SR * 2))


n_short, n_long = idle(5), idle(127)
check("delay noise floor rises at the longest times (%.0f dB -> %.0f dB)" % (db(n_short), db(n_long)), n_long > 1.5 * n_short)

# shared feedback loop: past the middle of FB it self-oscillates after the input is gone; below it dies away
def ring(fb):
    l = Lira(DEL_MIX=127, DST_MIX=0, TIME1=40, TIME2=55, FEEDBACK=fb, DRV=0, FAST12=1, FAST34=1, FAST56=1, FAST78=1)
    for k in range(8):
        l.key(k, 1)
    l.run(SR)
    for k in range(8):
        l.key(k, 0)
    l.run(SR * 8)
    return rms(l.run(SR))


r_lo, r_hi = ring(30), ring(110)
check("feedback: dies away at 30/127 (%.0f dB), self-oscillates at 110/127 (%.0f dB)" % (db(r_lo), db(r_hi)), r_hi > 8 * r_lo and r_hi > 0.02)

# ---------------------------------------------------------------- whole instrument
l = Lira()
for i in range(8):
    l.key(i, 1)
y = l.run(SR * 10)
check("default patch, all touched: finite", bool(np.all(np.isfinite(y))))
check("default patch: bounded", np.max(np.abs(y)) <= 1.0001, "peak %.3f" % np.max(np.abs(y)))
check("default patch: audible and not clipping flat", 0.05 < rms(y[SR * 5:]) < 0.5, "rms %.3f" % rms(y[SR * 5:]))

rng = random.Random(7)
for trial in range(int(os.environ.get('LIRA_TRIALS','16'))):
    l = Lira(seed=trial + 1)
    for n in NAMES:
        if n in ("FAST12 FAST34 FAST56 FAST78 SWITCH TOTAL_FB ANDOR LINK LFO_WAV QUANTIZE VIBRATO".split()):
            l.set(n, rng.choice([0, 1]))
        elif n.startswith("SOURCE") or n == "DEL_MOD":
            l.set(n, rng.choice([0, 1, 2]))
        else:
            l.set(n, rng.choice([0, 127, rng.uniform(0, 127)]))
    for i in range(8):
        l.touch(i, rng.choice([0, 0.3, 1]))
    ys = []
    for _ in range(10):
        ys.append(l.run(SR))
        if rng.random() < 0.6:
            n = rng.choice(NAMES)
            l.set(n, rng.uniform(0, 127) if not n.startswith(("SOURCE", "FAST", "DEL_MOD", "VIBRATO")) else rng.choice([0, 1]))
    y = np.concatenate(ys)
    ok = bool(np.all(np.isfinite(y))) and np.max(np.abs(y)) <= 1.0001
    check("random settings %d" % trial, ok, "peak %.3f" % np.max(np.abs(y)))

# CPU
l = Lira()
for i in range(8):
    l.key(i, 1)
l.run(SR)
t = time.perf_counter()
l.run(SR * 4)
dt = time.perf_counter() - t
print("INFO host x86 speed: %.0fx realtime per instance at %d Hz (%.1f ns/sample)" % (4 / dt, SR, dt / (4 * SR) * 1e9))

sys.exit(1 if fails else 0)
