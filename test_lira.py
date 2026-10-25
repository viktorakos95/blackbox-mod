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
lib.lira_tune_for_midi.argtypes = [ctypes.c_int, ctypes.c_float]
lib.lira_tune_for_midi.restype = ctypes.c_float

# parameter ids, same order as the enum in lira.h
NAMES = ("TUNE1 TUNE2 TUNE3 TUNE4 TUNE5 TUNE6 TUNE7 TUNE8 FAST12 FAST34 FAST56 FAST78 SHARP12 SHARP34 SHARP56 SHARP78 "
         "MOD12 MOD34 MOD56 MOD78 SOURCE12 SOURCE34 SOURCE56 SOURCE78 PITCH1234 PITCH5678 HOLD1234 HOLD5678 SWITCH "
         "TOTAL_FB VIBRATO DRV DST_MIX VOL F_A F_B ANDOR LINK TIME1 TIME2 FEEDBACK DEL_MIX MOD1 MOD2 DEL_MOD LFO_WAV "
         "QUANTIZE EXT WAVE").split()
P = {n: i for i, n in enumerate(NAMES)}
assert len(NAMES) == 49

SR = 44100
DL = 44032


class Lira:
    def __init__(self, seed=1, **params):
        self.mem = ctypes.create_string_buffer(lib.lira_size())
        self.d1 = (ctypes.c_int16 * DL)()
        self.d2 = (ctypes.c_int16 * DL)()
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

# -60 dB in ~0.5 s (FAST) / ~11.5 s (slow, tau 3.3 s: half the hardware's, on request) means -40 dB in 2/3 of that
for fast, lo, hi, what in ((1, 0.2, 0.6, "FAST: about 0.35 s"), (0, 5.0, 10.0, "slow: ~7.5 s")):
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
    l.run(3 * SR)                                 # (the slow attack is ~0.6 s at full touch: well open after 3 s)
    return rms(l.run(SR // 4))


def attack_ms(fast, frac=0.7):
    """ms until voice 1's level (20 ms windows) reaches frac of its open level"""
    l = Lira(**{**CLEAN, "FAST12": fast})
    l.key(0, 1)
    lv = [rms(l.run(SR // 50)) for _ in range(150)]
    top = max(lv[-10:])
    return next(i for i, v in enumerate(lv) if v >= frac * top) * 20


a_fast, a_slow = attack_ms(1), attack_ms(0)
check("FAST opens with a snap (within the first 20 ms window), the slow envelope takes about half a second or more", a_fast == 0 and a_slow >= 400, "fast %d ms, slow %d ms" % (a_fast, a_slow))


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

# ---------------------------------------------------------------- closed voices are silent (3.1.AI: the bleed of the hardware is gone)
quiet = rms(Lira(DEL_MIX=0, DST_MIX=0, FAST12=1).run(SR))
check("nothing touched: silence (the bleed of a closed voice is gone): rms %.1e" % quiet, quiet < 1e-6)
hi_t = {f"TUNE{i}": 120 for i in range(1, 9)}
b_hi = rms(Lira(DEL_MIX=0, DST_MIX=0, **hi_t).run(SR))
check("... whatever the tuning (turning a pitch knob with the voices off makes no sound): rms %.1e" % b_hi, b_hi < 1e-6)

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
check("FM as in the plugin, f * (1 + mod): the same relative spread at any tuning (relative spread %.3f at %.0f Hz vs %.3f at %.0f Hz)" % (dev_lo, f_lo, dev_hi, f_hi),
      dev_lo > 0.01 and 0.5 < dev_hi / dev_lo < 2.0)

# ---------------------------------------------------------------- delay (PT2399 model)
def td_of(knob):
    return 0.031 * 2 ** (7.5826 * knob / 127)


def delay_lag(knob, secs):
    kw = {**CLEAN, "TIME1": knob, "TIME2": knob, "FEEDBACK": 0, "MOD1": 0, "MOD2": 0}
    dry = Lira(**{**kw, "DEL_MIX": 0})
    wet = Lira(**{**kw, "DEL_MIX": 127, "DRV": 0})
    for l in (dry, wet):
        l.run(SR // 5)                                   # the knobs settle first (their glide from the defaults leaks a little dry)
        l.touch(0, 0.5)                                  # a slow, partial attack gives a clear onset
    yd, yw = dry.run(SR * secs), wet.run(SR * secs)
    return onset(yw, frac=0.25) - onset(yd, frac=0.25)   # (the echo first comes in at about half its final level: the codec's compander settles)


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

# ---- sine waveform and exact tuning ----
def level_at(x, f, n=1 << 16):
    sp, fr = spectrum(x, n)
    k = np.argmin(abs(fr - f))
    return sp[max(0, k - 3):k + 4].max()


def one_voice(wave, note, sharp=0):
    l = Lira(**{**CLEAN, "SHARP12": sharp, "WAVE": wave, "PITCH1234": 63.1809})
    l.set("TUNE1", lib.lira_tune_for_midi(0, note))
    l.key(0, 1)
    l.run(SR)
    return l.run(1 << 16)


tri = one_voice(0, 57)
sin = one_voice(1, 57)
f0 = mtof(57)
h3_tri = db(level_at(tri, 3 * f0) / level_at(tri, f0))
h3_sin = db(level_at(sin, 3 * f0) / level_at(sin, f0))
check("sine mode: 3rd harmonic %.0f dB (the triangle's: %.0f dB, theory -19)" % (h3_sin, h3_tri), h3_tri > -24 and h3_sin < -46)   # (-50 until 3.1.AO: the EQ stage's bass lift drives the master's tanh a little harder)
h5_sin = db(level_at(sin, 5 * f0) / level_at(sin, f0))
check("sine mode: 5th harmonic %.0f dB" % h5_sin, h5_sin < -55)
for note in (36, 48, 57, 60, 72, 84, 93):
    y = one_voice(1, note)
    got = peak_hz(y, 20, 8000)
    cents = 1200 * math.log2(got / mtof(note))
    check("tune_for_midi: note %d sounds %.1f Hz, %.1f cents off" % (note, got, cents), abs(cents) < 4)
for o in range(8):                                                  # every oscillator reaches the shared range
    check("oscillator %d takes notes 33 and 93 (tune within 0..127)" % (o + 1),
          0.0 < lib.lira_tune_for_midi(o, 33) + 1e-3 and lib.lira_tune_for_midi(o, 93) < 127.001)


# ---- SHARP is a morph (3.1.AC): the sine is squared off gradually, no second waveform added ----
def sharp_voice(sharp):
    l = Lira(**dict(CLEAN, WAVE=1, SHARP12=sharp))
    l.key(0, 1)
    l.run(SR)
    return l.run(1 << 16)


sh_levels = {sp: sharp_voice(sp) for sp in (0, 20, 40, 60, 80, 100)}
f_sh = peak_hz(sh_levels[0], 20, 8000)


def harm_db(x, n):
    return db(level_at(x, n * f_sh) / level_at(x, f_sh))


check("SHARP low: the sine stays clean (5th below -80 dB at 0 and 20)", harm_db(sh_levels[0], 5) < -80 and harm_db(sh_levels[20], 5) < -75)
check("SHARP rising: the 3rd harmonic comes first (40: 3rd above -30 dB, 5th below -45, 7th below -65)",
      harm_db(sh_levels[40], 3) > -30 and harm_db(sh_levels[40], 5) < -45 and harm_db(sh_levels[40], 7) < -65,
      "%.0f %.0f %.0f" % (harm_db(sh_levels[40], 3), harm_db(sh_levels[40], 5), harm_db(sh_levels[40], 7)))
check("... and the upper ones follow smoothly (the 5th rises with every step)", [harm_db(sh_levels[k], 5) for k in (40, 60, 80, 100)] ==
      sorted(harm_db(sh_levels[k], 5) for k in (40, 60, 80, 100)))
rm = {k: math.sqrt(np.mean(v ** 2)) for k, v in sh_levels.items()}
check("the level stays within 1.5 dB while it squares off", max(rm.values()) / min(rm.values()) < 1.19, str({k: round(v, 3) for k, v in rm.items()}))

# ---- stereo and drift (3.1.AC) ----
import struct as _st
_o = os.path.join(tmp, "off")
open(_o + ".c", "w").write('#include "lira.h"\n#include <stddef.h>\n#include <stdio.h>\nint main(){printf("%d",(int)offsetof(lira_t,out_r));}')
subprocess.check_call(["gcc", "-I" + os.path.join(HERE, "src"), "-o", _o, _o + ".c"])
OUT_R = int(subprocess.check_output([_o]))
lib.lira_tone.argtypes = [ctypes.c_void_p] + [ctypes.c_float] * 2


def run_st(l, n, tone, stereo=True):
    lib.lira_tone(l.mem, *tone)
    a, b = (ctypes.c_float * n)(), (ctypes.c_float * n)()
    ptr = ctypes.c_void_p(ctypes.addressof(b) if stereo else 0)
    ctypes.memmove(ctypes.addressof(l.mem) + OUT_R, ctypes.byref(ptr), ctypes.sizeof(ptr))
    lib.lira_process(l.mem, a, n)
    return np.frombuffer(a, dtype=np.float32).astype(np.float64), np.frombuffer(b, dtype=np.float32).astype(np.float64)


def full_drone():
    l = Lira(DEL_MIX=60, FEEDBACK=50, DST_MIX=20, VIBRATO=0, FAST12=1, FAST34=1, FAST56=1, FAST78=1)
    for i in range(8):
        l.key(i, 1)
    return l


l = full_drone()
run_st(l, SR, (0, 0))
a, b = run_st(l, SR, (0, 0))
check("width 0: left = right", np.array_equal(a, b) and np.std(a) > 0.01)
l = full_drone()
run_st(l, SR, (1, 0))
a, b = run_st(l, 2 * SR, (1, 0))
mid, side = (a + b) / 2, (a - b) / 2
ratio = math.sqrt(np.mean(side ** 2) / np.mean(mid ** 2))
check("width 1: a real stereo image (side %.2f of mid, correlation %.2f)" % (ratio, np.corrcoef(a, b)[0, 1]), 0.25 < ratio < 1.0 and np.corrcoef(a, b)[0, 1] < 0.85)
check("... and balanced: the voices are dealt out so neither side is crowded (L / R level %.2f)" % (np.sqrt(np.mean(a ** 2) / np.mean(b ** 2))), 0.8 < np.sqrt(np.mean(a ** 2) / np.mean(b ** 2)) < 1.25)
check("... finite and bounded", np.all(np.isfinite(a)) and np.all(np.isfinite(b)) and max(abs(a).max(), abs(b).max()) < 1.5)
l1, l2 = full_drone(), full_drone()
run_st(l1, SR, (0.8, 0))
run_st(l2, SR, (0.8, 0), stereo=False)
a, _ = run_st(l1, 8192, (0.8, 0))
c, d = run_st(l2, 8192, (0.8, 0), stereo=False)
check("with no right buffer the engine stays mono (a pad's instance: no crash, a signal, nothing written)", np.std(c) > 0.01 and np.all(d == 0))


def pitch_track(drift, sec=40):
    l = Lira(**dict(CLEAN, WAVE=1, DEL_MIX=0))
    l.key(0, 1)
    x = np.concatenate([run_st(l, SR, (0, drift), stereo=False)[0] for _ in range(sec)])
    f = []
    for i in range(15, sec):                                        # after the envelope has settled
        seg = x[i * SR:(i + 1) * SR]
        zc = np.where((seg[:-1] < 0) & (seg[1:] >= 0))[0]
        f.append(SR * (len(zc) - 1) / (zc[-1] - zc[0]))
    return np.array(f)


f_off, f_half, f_full = (pitch_track(d) for d in (0.0, 0.5, 1.0))
c_off, c_half, c_full = (1200 * math.log2(f.max() / f.min()) for f in (f_off, f_half, f_full))
check("drift off: the pitch is steady (%.2f cents)" % c_off, c_off < 0.2)
check("drift 0.5 wanders a few cents (%.1f), full more (%.1f), none beyond 25" % (c_half, c_full), 1.0 < c_half < c_full < 25.0)


# ---- the delay's clock moves the pitch of the echoes, it does not move the read pointer (3.1.AI: no tick at a square LFO's edges) ----
def clicks(x):
    h = np.diff(x, 2)
    n = int(SR * 0.002)
    e = np.array([np.sqrt(np.mean(h[i:i + n] ** 2)) for i in range(0, len(h) - n, n)])
    return e.max() / np.median(e)


l = Lira(DEL_MIX=90, FEEDBACK=60, DST_MIX=0, VIBRATO=0, DEL_MOD=2, LFO_WAV=0, MOD1=127, MOD2=127, F_A=70, F_B=70, TIME1=40, TIME2=50)
for i in range(4):
    l.key(i, 1)
l.run(6 * SR)
tri_part = l.run(SR // 2)
l.set("LFO_WAV", 1)
sq_part = l.run(SR // 2)
check("LFO square on the delay clock: no clicks (peak / median of the 2nd-difference energy %.1f, was 44; the triangle's %.1f)" % (clicks(sq_part), clicks(tri_part)),
      clicks(sq_part) < 12)


# ---- FM Source OUT (8): the master output itself modulates the pair (3.1.AJ) ----
def src_run(src, mod=60):
    l = Lira(**dict(CLEAN, WAVE=0, DEL_MIX=0, DST_MIX=20, MOD12=mod, SOURCE12=src))
    l.key(0, 1)
    l.key(1, 1)
    l.run(SR)
    return l.run(2 * SR)


x_off, x_out, x_lfo = src_run(1), src_run(8), src_run(2)
check("FM Source OUT: finite, bounded, and not the same as Off or LFO (rms %.3f / %.3f / %.3f)" % (rms(x_out), rms(x_off), rms(x_lfo)),
      np.all(np.isfinite(x_out)) and abs(x_out).max() < 1.5 and not np.allclose(x_out, x_off, atol=1e-4) and not np.allclose(x_out, x_lfo, atol=1e-4))
check("... and with no depth it does nothing (the same as Off)", np.allclose(src_run(8, 0), src_run(1, 0), atol=1e-6))


# ---- QUANTIZE: every voice sounds an exact equal-tempered note ----
def quant_cents(tune, quant):
    l = Lira(**dict(CLEAN, WAVE=1, DEL_MIX=0, TUNE1=tune, QUANTIZE=quant, PITCH1234=63))
    l.key(0, 1)
    l.run(8 * SR)
    f = peak_hz(l.run(4 * SR), 20, 4000)
    m = 69 + 12 * math.log2(f / 440.0)
    return abs(m - round(m)) * 100


offs_q = [quant_cents(t, 1) for t in (60, 67, 74, 83, 91, 104)]
offs_f = [quant_cents(t, 0) for t in (60, 67, 74, 83, 91, 104)]
check("QUANTIZE on: every tune lands on a note (worst %.1f cents off: the oscillator's own accuracy)" % max(offs_q), max(offs_q) < 6.0)
check("... off: the knob is continuous (some are far from a note: %.0f cents)" % max(offs_f), max(offs_f) > 15.0)


# ---- the delay clock's wobble (3.1.AN): a drone and its echo stop cancelling in the same places; a tempo-synced line keeps its time ----
_o2 = os.path.join(tmp, "off2")
open(_o2 + ".c", "w").write('#include "lira.h"\n#include <stddef.h>\n#include <stdio.h>\nint main(){printf("%d %d",(int)offsetof(lira_t,dsync),(int)offsetof(lira_t,line[0].wob));}')
subprocess.check_call(["gcc", "-I" + os.path.join(HERE, "src"), "-o", _o2, _o2 + ".c"])
OFF_DSYNC, OFF_WOB = (int(v) for v in subprocess.check_output([_o2]).split())


def echo_level(drift, dsync=0, tune=100, secs=24):
    l = Lira(DST_MIX=0, VIBRATO=0, FAST12=1, MOD12=0, SOURCE12=1, SHARP12=0, WAVE=1, PITCH1234=63, TUNE1=tune, DEL_MIX=64, FEEDBACK=0,
             MOD1=0, MOD2=0, DEL_MOD=1, TIME1=70, TIME2=70)
    l.key(0, 1)
    l.run(8 * SR)
    ctypes.c_uint8.from_address(ctypes.addressof(l.mem) + OFF_DSYNC).value = dsync
    lib.lira_tone(l.mem, 0, drift)
    x = l.run(secs * SR)
    wob = ctypes.c_float.from_address(ctypes.addressof(l.mem) + OFF_WOB).value
    return x, wob


_lr = Lira(DST_MIX=0, VIBRATO=0, FAST12=1, MOD12=0, SOURCE12=1, SHARP12=0, WAVE=1, PITCH1234=63, TUNE1=100, DEL_MIX=0)
_lr.key(0, 1)
_lr.run(8 * SR)
ref = rms(_lr.run(8 * SR))
x_still, w_still = echo_level(0.0)
x_wob, w_wob = echo_level(0.4)
x_syn, w_syn = echo_level(0.4, dsync=3)
lv = lambda x: 20 * np.log10(rms(x[SR:]) / ref)
check("no drift: the tone and its echo cancel for good at this pitch and time (%.0f dB)" % lv(x_still), lv(x_still) < -25)
check("DRIFT 0.4: the cancellation moves on, the average level comes back (%.0f dB)" % lv(x_wob), lv(x_wob) > -14 and 0 < abs(w_wob) < 0.06)
check("a tempo-synced line keeps its time exactly (wobble %.5f)" % w_syn, abs(w_syn) < 1e-6)


# ---- the Hyper LFO's AND (3.1.AO): logic squares, high only while both are; OR / SUM their sum, three levels ----
def lfo_pitch(andor, fa=40, fb=55):
    l = Lira(**dict(CLEAN, WAVE=1, MOD12=90, SOURCE12=2, ANDOR=andor, F_A=fa, F_B=fb))
    l.key(0, 1)
    l.run(2 * SR)
    x = l.run(8 * SR)
    w, f = int(SR * 0.01), []
    for i in range(0, len(x) - w, w):
        zc = np.where((x[i:i + w - 1] < 0) & (x[i + 1:i + w] >= 0))[0]
        f.append(SR * (len(zc) - 1) / (zc[-1] - zc[0]) if len(zc) > 2 else 0.0)
    return np.array(f)


fa_, fo_ = lfo_pitch(0), lfo_pitch(1)
lo_, hi_ = np.percentile(fa_, 2), np.percentile(fa_, 98)
up_and = np.mean(fa_ > (lo_ + hi_) / 2)
lv_or = [np.mean(abs(fo_ - (lo_ + k * (hi_ - lo_) / 2)) < 0.1 * (hi_ - lo_)) for k in range(3)]   # time near each of the three levels
check("AND: the voice rests at its own pitch and jumps only while both squares are high (up %.0f %% of the time, a quarter expected; never below %.0f Hz)" % (100 * up_and, lo_),
      0.15 < up_and < 0.35 and fa_.min() > 0.9 * lo_)
check("OR / SUM: three levels (0, 1/2, 1: %.0f / %.0f / %.0f %% of the time), little in between" % tuple(100 * v for v in lv_or),
      min(lv_or) > 0.03 and sum(lv_or) > 0.9)


# ---- the drive stage (3.1.AO): more DRIVE gives more bass and less top, as the real one does; the low end reaches the clipper ----
def band_db(x, a, b):
    X = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    f = np.fft.rfftfreq(len(x), 1 / SR)
    return 10 * np.log10(X[(f >= a) & (f < b)].sum() + 1e-30)


def driven(drv):
    l = Lira(DEL_MIX=0, VIBRATO=0, DST_MIX=127, DRV=drv, FAST12=1, FAST34=1, FAST56=1, FAST78=1, MOD12=0, MOD34=0, MOD56=0, MOD78=0)
    for i in range(8):
        l.key(i, 1)
    l.run(2 * SR)
    return l.run(3 * SR)


x0_, x1_ = driven(0), driven(127)
db_b, db_h = band_db(x1_, 20, 200) - band_db(x0_, 20, 200), band_db(x1_, 3000, 20000) - band_db(x0_, 3000, 20000)
check("DRIVE full: more bass (%+.1f dB below 200 Hz) and no more top (%+.1f dB above 3 kHz)" % (db_b, db_h), db_b > 1.5 and db_h < 0.5)
check("... finite, bounded, no DC (peak %.2f, mean %.4f)" % (abs(x1_).max(), x1_.mean()),
      np.all(np.isfinite(x1_)) and abs(x1_).max() < 1.2 and abs(x1_.mean()) < 0.01)


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
