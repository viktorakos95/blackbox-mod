"""Tonal spaces and voicings (src/lira_chord.c) on the host. Checked: every space's pitch classes all sound, the root is the
lowest note, notes stay in the range every oscillator reaches, the same seed repeats and different seeds differ, the notes belong
to the space, 8 voices always, just-intonation offsets, note names."""
import ctypes
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
so = os.path.join(tempfile.mkdtemp(), "chord.so")
subprocess.check_call(["gcc", "-O2", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror", "-o", so, os.path.join(HERE, "src", "lira_chord.c")])
lib = ctypes.CDLL(so)
lib.lira_space_name.restype = ctypes.c_char_p
lib.lira_just_cents.restype = ctypes.c_float
lib.lira_voicing.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint32, ctypes.c_void_p]
fails = 0


def check(name, ok, info=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + ("  " + info if info else ""))
    fails += not ok


def voicing(space, root, low, span, seed):
    out = (ctypes.c_int8 * 8)()
    lib.lira_voicing(space, root, low, span, seed, out)
    return list(out)


N = lib.lira_space_count()
check("%d spaces" % N, N >= 28)
PCS = {}
import re
src = open(os.path.join(HERE, "src", "lira_chord.c")).read()
for m in re.finditer(r'\{"([^"]+)",\s*([^}]+)\}', src):
    PCS[m.group(1)] = sorted({int(x) for x in re.findall(r"B\((\d+)\)", m.group(2))})
check("names and tables match", all(lib.lira_space_name(i).decode() in PCS for i in range(N)))
bad = []
for sp in range(N):
    name = lib.lira_space_name(sp).decode()
    pcs = PCS[name]
    for root in (0, 5, 9):
        for low in (33, 36, 48, 60):
            for span in (12, 24, 36, 48):
                for seed in range(6):
                    v = voicing(sp, root, low, span, seed)
                    rp = low + (root - low) % 12
                    hi = max(min(93, low + span), min(93, rp + 11))
                    got = {(n - root) % 12 for n in v}
                    want = set(pcs) if len(pcs) <= 8 else None
                    ok = (len(v) == 8 and all(33 <= n <= 93 for n in v) and all(rp <= n <= hi for n in v)
                          and all((n - root) % 12 in pcs for n in v) and (want is None or want <= got)
                          and min(v) == rp)
                    if not ok:
                        bad.append((name, root, low, span, seed, v))
check("every voicing: 8 notes in range, in the space, all pitch classes present, root is the lowest", not bad, str(bad[:2]))
check("the same seed gives the same voicing", voicing(0, 0, 36, 36, 7) == voicing(0, 0, 36, 36, 7))
distinct = {tuple(voicing(0, 0, 36, 36, s)) for s in range(40)}
check("different seeds give different voicings (%d of 40)" % len(distinct), len(distinct) >= 30)
check("an 8-pitch-class space gives eight different notes", len(set(voicing(28, 0, 33, 60, 3))) == 8)
check("just offsets: fifth +2.0, major third -13.7, minor third +15.6", abs(lib.lira_just_cents(7, 0) - 1.96) < .01 and
      abs(lib.lira_just_cents(4, 0) + 13.69) < .01 and abs(lib.lira_just_cents(3, 0) - 15.64) < .01)
check("just offsets are relative to the root", abs(lib.lira_just_cents(9 + 12, 2) - 1.96) < .01)
buf = ctypes.create_string_buffer(5)
names = []
for n in (36, 61, 93, 33):
    lib.lira_note_name(n, buf)
    names.append(buf.value.decode())
check("note names", names == ["C2", "C#4", "A6", "A1"], str(names))
print("failures:", fails)
sys.exit(1 if fails else 0)
