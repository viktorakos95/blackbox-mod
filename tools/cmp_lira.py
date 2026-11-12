"""cmp_lira.py [rev]: the engine in the working tree (src/lira.c) against git `rev` (default HEAD), both built for the host,
fed the same scenarios (all voices, sine, slow delay, FM sources, key changes mid-run).

The voices modulate each other through feedback, so a change of rounding alone (the same old source built with fused
multiply-adds) drifts apart audibly-in-the-waveform within a second while sounding the same. So each scenario prints,
for the new build and for that rounding-only build: the sample where they first differ by 1e-6 and by 1e-3, and the
level of the last second. A rewrite for speed should drift no sooner than the rounding-only build and keep the level."""
import ctypes
import os
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
rev = sys.argv[1] if len(sys.argv) > 1 else "HEAD"
tmp = tempfile.mkdtemp()
old_c = os.path.join(tmp, "old.c")
open(old_c, "w").write(subprocess.run(["git", "show", rev + ":src/lira.c"], cwd=HERE, capture_output=True, text=True, check=True).stdout)
open(os.path.join(tmp, "lira.h"), "w").write(subprocess.run(["git", "show", rev + ":src/lira.h"], cwd=HERE, capture_output=True, text=True, check=True).stdout)


def build(src, name, inc, extra=()):
    so = os.path.join(tmp, name)
    subprocess.check_call(["gcc", "-O2", "-ffp-contract=off", *extra, "-shared", "-fPIC", "-I", inc, "-o", so, src])
    lib = ctypes.CDLL(so)
    lib.lira_size.restype = ctypes.c_int
    lib.lira_init.argtypes = [ctypes.c_void_p, ctypes.c_float, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_uint32]
    lib.lira_param.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_float]
    lib.lira_touch.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_float]
    lib.lira_process.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
    return lib


OLD = build(old_c, "old.so", tmp)
FMA = build(old_c, "fma.so", tmp, ["-mfma", "-ffp-contract=fast"])
NEW = build(os.path.join(HERE, "src", "lira.c"), "new.so", os.path.join(HERE, "src"))
NAMES = ("TUNE1 TUNE2 TUNE3 TUNE4 TUNE5 TUNE6 TUNE7 TUNE8 FAST12 FAST34 FAST56 FAST78 SHARP12 SHARP34 SHARP56 SHARP78 "
         "MOD12 MOD34 MOD56 MOD78 SOURCE12 SOURCE34 SOURCE56 SOURCE78 PITCH1234 PITCH5678 HOLD1234 HOLD5678 SWITCH "
         "TOTAL_FB VIBRATO DRV DST_MIX VOL F_A F_B ANDOR LINK TIME1 TIME2 FEEDBACK DEL_MIX MOD1 MOD2 DEL_MOD LFO_WAV "
         "QUANTIZE EXT WAVE LVL1 LVL2 LVL3 LVL4 LVL5 LVL6 LVL7 LVL8").split()
P = {n: i for i, n in enumerate(NAMES)}
DL, SR, N = 44032, 48000.0, 128
SHED_OFF = None


def run(lib, scen):
    mem = ctypes.create_string_buffer(lib.lira_size() + 64)
    d1, d2 = (ctypes.c_int16 * DL)(), (ctypes.c_int16 * DL)()
    lib.lira_init(mem, SR, d1, d2, DL, 7)
    ext = (ctypes.c_float * N)()
    rng = np.random.default_rng(3)
    out = []
    for b, ev in enumerate(scen):
        for kind, a, v in ev:
            if kind == "p":
                lib.lira_param(mem, P[a], float(v))
            elif kind == "t":
                lib.lira_touch(mem, a, float(v))
            elif kind == "x":         # ext_l/ext_r/ext_n are set through the struct: offsets found by the caller
                pass
        o = (ctypes.c_float * N)()
        lib.lira_process(mem, o, N)
        out.append(np.frombuffer(o, dtype=np.float32).copy())
    return np.concatenate(out)


def scen(params, keys, blocks=1500, later=()):
    s = [[] for _ in range(blocks)]
    s[0] = [("p", k, v) for k, v in params.items()] + [("t", k, 1.0) for k in keys]
    for b, ev in later:
        s[b] += ev
    return s


import os as _os
_SH = int(_os.environ.get("SHARP", "40"))                          # SHARP=0 compares the engine where the knob is off (the morph changes the rest on purpose)
BASE = {"FEEDBACK": 70, "DEL_MIX": 80, "MOD12": 60, "MOD34": 60, "MOD56": 60, "MOD78": 60, "SHARP12": _SH, "SHARP34": _SH, "SHARP56": _SH, "SHARP78": _SH,
        "DST_MIX": 60, "MOD1": 40, "MOD2": 40}
SCEN = {
    "all voices, tri": scen(BASE, range(8)),
    "all voices, sine": scen(dict(BASE, WAVE=1), range(8)),
    "wave switched mid-run": scen(BASE, range(8), later=[(300, [("p", "WAVE", 1)]), (700, [("p", "WAVE", 0)]), (1000, [("p", "WAVE", 1)])]),
    "slow delay": scen(dict(BASE, TIME1=127, TIME2=120), range(8)),
    "FM sources LFO / fb / pairs": scen(dict(BASE, SOURCE12=2, SOURCE34=4, SOURCE56=7, SOURCE78=0, SWITCH=1, TOTAL_FB=1), range(8)),
    "FM source LFO, square, AND": scen(dict(BASE, SOURCE12=2, SOURCE34=2, ANDOR=0, LINK=1, DEL_MOD=2, LFO_WAV=1), range(8)),
    "keys on and off": scen(BASE, (0, 3, 5), later=[(200, [("t", 0, 0.0), ("t", 6, 0.6)]), (600, [("t", 3, 0.0), ("t", 5, 0.0)]),
                                                     (900, [("t", 1, 1.0)])]),
    "idle, hold": scen(dict(BASE, HOLD1234=100), ()),
    "silent": scen(BASE, ()),
}


def drift(a, b):
    d = np.abs(a - b)
    return [int(np.argmax(d > t)) if (d > t).any() else -1 for t in (1e-6, 1e-3)]


bad = 0
print("%-30s %24s %24s %18s" % ("", "new: drift at 1e-6, 1e-3", "rounding: 1e-6, 1e-3", "level old / new"))
for name, s in SCEN.items():
    a, b, c = (run(lib, s).astype(np.float64) for lib in (OLD, NEW, FMA))
    dn, dr = drift(a, b), drift(a, c)
    ra, rb = (np.sqrt(np.mean(x[-48000:] ** 2)) for x in (a, b))
    ok = np.all(np.isfinite(b)) and abs(rb - ra) <= 0.02 * ra + 1e-6
    bad += not ok
    print("%-30s %12d %11d %12d %11d %9.4f %8.4f%s" % (name, *dn, *dr, ra, rb, "" if ok else "  LEVEL / NOT FINITE"))
print("level kept in every scenario" if not bad else "%d scenarios changed level" % bad)
