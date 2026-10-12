"""LIRA-8 engine (src/lira.c) on the host: compiled with the system gcc and driven through ctypes.

Checks: silence with no touch, oscillator pitch against the tune/pitch formulas, touch-envelope timing, bounded finite
output under random extreme settings, and CPU cost per instance.
NOT checked: how it sounds against the Pd original (no Pd here), or anything on the device.
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
lib.lira_process.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]

# parameter ids, same order as the enum in lira.h
NAMES = ("TUNE1 TUNE2 TUNE3 TUNE4 TUNE5 TUNE6 TUNE7 TUNE8 FAST12 FAST34 FAST56 FAST78 SHARP12 SHARP34 SHARP56 SHARP78 "
         "MOD12 MOD34 MOD56 MOD78 SOURCE12 SOURCE34 SOURCE56 SOURCE78 PITCH1234 PITCH5678 HOLD1234 HOLD5678 SWITCH "
         "TOTAL_FB VIBRATO DRV DST_MIX VOL F_A F_B ANDOR LINK TIME1 TIME2 FEEDBACK DEL_MIX MOD1 MOD2 DEL_MOD LFO_WAV "
         "QUANTIZE").split()
P = {n: i for i, n in enumerate(NAMES)}
assert len(NAMES) == 47

SR = 44100
DL = 1 << 18


class Lira:
    def __init__(self, seed=1):
        self.mem = ctypes.create_string_buffer(lib.lira_size())
        self.d1 = (ctypes.c_float * DL)()
        self.d2 = (ctypes.c_float * DL)()
        lib.lira_init(self.mem, SR, self.d1, self.d2, DL, seed)

    def set(self, name, v):
        lib.lira_param(self.mem, P[name], v)

    def key(self, i, on):
        lib.lira_key(self.mem, i, on)

    def run(self, n):
        out = (ctypes.c_float * n)()
        lib.lira_process(self.mem, out, n)
        return np.frombuffer(out, dtype=np.float32).copy()


def mtof(m):
    return 440.0 * 2 ** ((m - 69) / 12)


def peak_hz(x, lo=20, hi=8000):
    w = x * np.hanning(len(x))
    spec = np.abs(np.fft.rfft(w, 1 << 18))
    f = np.fft.rfftfreq(1 << 18, 1 / SR)
    m = (f > lo) & (f < hi)
    return f[m][np.argmax(spec[m])]


fails = 0


def check(name, ok, info=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + ("  " + info if info else ""))
    fails += not ok


# 1. no touch, no hold: silence
l = Lira()
y = l.run(SR)
check("silent without touch", np.max(np.abs(y)) < 1e-3, "peak %.2e" % np.max(np.abs(y)))

# 2. pitch of osc 1: dry, no FM, no vibrato, no drive colouring, delay off
l = Lira()
for k, v in dict(DEL_MIX=0, DST_MIX=0, VIBRATO=0, FAST12=1, SHARP12=0, MOD12=0, SOURCE12=1, PITCH1234=127, TUNE1=100).items():
    l.set(k, v)
l.key(0, 1)
l.run(SR)                                   # let the touch envelope open
y = l.run(SR * 2)
want = mtof(-16 + 100 / 127 * (93 + 16)) * (127 / 127 * 1.99 + 0.01)
got = peak_hz(y)
check("osc 1 pitch", abs(got / want - 1) < 0.01, "want %.1f Hz got %.1f Hz" % (want, got))
check("osc 1 audible, bounded", 0.01 < np.max(np.abs(y)) <= 1.0, "peak %.3f" % np.max(np.abs(y)))

# 3. quantize snaps to the nearest semitone
l.set("QUANTIZE", 1)
l.run(SR // 4)
y = l.run(SR * 2)
m = 69 + 12 * math.log2(want / 440)
want_q = mtof(round(m))
check("quantize", abs(peak_hz(y) / want_q - 1) < 0.01, "want %.1f Hz got %.1f Hz" % (want_q, peak_hz(y)))

# 4. touch envelope: fast = 100 ms up, 100 ms down; slow = 200 ms up, 8 s down
def level(x):
    return float(np.sqrt(np.mean(x * x)))


for fast, rel_ms in ((1, 100), (0, 8000)):
    l = Lira()
    for k, v in dict(DEL_MIX=0, DST_MIX=0, VIBRATO=0, FAST12=fast, MOD12=0, SOURCE12=1, TUNE1=100, TUNE2=0).items():
        l.set(k, v)
    l.key(0, 1)
    l.run(SR // 20)
    early = level(l.run(SR // 50))
    l.run(SR)
    full = level(l.run(SR // 20))
    l.key(0, 0)
    l.run(int(SR * rel_ms / 1000 * 0.5))
    mid = level(l.run(SR // 50))
    l.run(int(SR * rel_ms / 1000 * 1.2))
    end = level(l.run(SR // 20))
    check("touch envelope (fast=%d)" % fast, early < 0.5 * full and end < 0.05 * full and 0.05 * full < mid,
          "early/full %.3f  mid/full %.3f  end/full %.4f" % (early / full, mid / full, end / full))

# 5. a full default patch, all eight touched: finite, bounded, not silent, and the delay tails decay
l = Lira()
for i in range(8):
    l.key(i, 1)
y = l.run(SR * 10)
check("default patch, all touched: finite", bool(np.all(np.isfinite(y))))
check("default patch: bounded", np.max(np.abs(y)) <= 1.0001, "peak %.3f" % np.max(np.abs(y)))
check("default patch: audible", level(y[SR * 5:]) > 0.02, "rms %.3f" % level(y[SR * 5:]))
for i in range(8):
    l.key(i, 0)
l.run(SR * 20)
tail = l.run(SR)
check("tail settles after release", level(tail) < 0.05, "rms %.4f" % level(tail))

# 6. random extreme settings
rng = random.Random(7)
for trial in range(12):
    l = Lira(seed=trial + 1)
    for n in NAMES:
        if n in ("FAST12 FAST34 FAST56 FAST78 SWITCH TOTAL_FB ANDOR LINK LFO_WAV QUANTIZE".split()):
            l.set(n, rng.choice([0, 1]))
        elif n.startswith("SOURCE") or n == "DEL_MOD":
            l.set(n, rng.choice([0, 1, 2]))
        else:
            l.set(n, rng.choice([0, 127, rng.uniform(0, 127)]))
    for i in range(8):
        l.key(i, rng.choice([0, 1]))
    ys = []
    for _ in range(8):
        ys.append(l.run(SR))
        if rng.random() < 0.5:
            n = rng.choice(NAMES)
            l.set(n, rng.uniform(0, 127) if not n.startswith(("SOURCE", "FAST")) else rng.choice([0, 1]))
    y = np.concatenate(ys)
    ok = bool(np.all(np.isfinite(y))) and np.max(np.abs(y)) <= 1.0001
    check("random settings %d" % trial, ok, "peak %.3f" % np.max(np.abs(y)))

# 7. CPU
l = Lira()
for i in range(8):
    l.key(i, 1)
l.run(SR)
t = time.perf_counter()
l.run(SR * 4)
dt = time.perf_counter() - t
print("INFO host x86 speed: %.0fx realtime per instance at %d Hz (%.1f ns/sample)" % (4 / dt, SR, dt / (4 * SR) * 1e9))

sys.exit(1 if fails else 0)
