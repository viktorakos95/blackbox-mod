#!/usr/bin/env python3
"""Renders the pictures the manual uses (every Looper tab, SAMPLR sheet, DRONE-8 tab and Trigger2MIDI sheet) from the mod's own page drawing code in the emulator,
on the `all` image, into a folder; then `python3 tools/manual_build.py <folder>` turns them into docs/img/manual and docs/manual.json.

  python3 tools/manual_shots.py [folder]       (default /tmp/manual_shots)

The Looper is shown recording (track 1 REC, tracks 2 and 3 playing) and SAMPLR with a drum-loop waveform, both made of test sounds. The pictures are renders, not photos:
for the pictures only, the SAVE button's state query is made to report idle (the emulator has no DRONE pool, so it would read FAILED).
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import combo as C  # noqa: E402

OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/manual_shots"
os.chdir(ROOT)
C.ensure_cave()
C.build(C.resolve(["all"]))
img = os.path.join(ROOT, "out", "combos", "all", "BLACKBOX.bin")
os.makedirs(os.path.join(ROOT, "out", "combos", "all"), exist_ok=True)
shutil.copy2(os.path.join(ROOT, "out", "+".join(C.resolve(["all"])), "BLACKBOX.bin"), img)
sets = C.resolve(["all"])
shutil.rmtree(OUT, ignore_errors=True)
os.makedirs(OUT)

HELPER = '''
import math as _m
_seed = [12345]
def _noise():
    _seed[0] = (_seed[0] * 1103515245 + 12345) & 0x7fffffff
    return (_seed[0] / 0x3fffffff) - 1.0
def drum(k):
    p = k % 48000; v = 0.0
    for hit, kind in ((0, 'k'), (12000, 's'), (24000, 'k'), (36000, 's')):
        t = p - hit
        if t >= 0:
            if kind == 'k': v += 0.8 * _m.exp(-t / 2500.0) * _m.sin(2 * _m.pi * (55 + 90 * _m.exp(-t / 600.0)) * t / 48000.0)
            else: v += 0.6 * _m.exp(-t / 2200.0) * _noise() * 0.8 + 0.25 * _m.exp(-t / 1500.0) * _m.sin(2 * _m.pi * 190 * t / 48000.0)
    for h in range(6000, 48000, 12000):
        t = p - h
        if t >= 0: v += 0.18 * _m.exp(-t / 500.0) * _noise()
    return max(-0.95, min(0.95, v))
def idle_save():
    e.uc.mem_write(e.sym["lira_ui_save"] & ~1, bytes.fromhex("00207047"))     # movs r0,#0 ; bx lr
    e.uc.ctl_flush_tb()
'''
src = open("test_looper.py").read()
a = src.index('snap("samplr")') + len('snap("samplr")')
samplr = src[:a].replace("vals = [(start + i) / LENF if 0 <= start + i < LENF else 0.0 for i in range(n)]", "vals = [drum((start + i) * 3) if 0 <= start + i < LENF else 0.0 for i in range(n)]") \
    .replace('struct.pack("<8192f", *[(b_ * 8192 + i) / LENF for i in range(8192)])', 'struct.pack("<8192f", *[drum((b_ * 8192 + i) * 3) for i in range(8192)])').replace("fails = 0", HELPER + "\nfails = 0", 1)
samplr += '''
idle_save()
draw(0); snap("samplr_play")
for sheet, name in ((1, "samplr_rec"), (2, "samplr_fx"), (3, "samplr_smpl")):
    e.uc.mem_write(PAGE + 18, struct.pack("<H", sheet)); draw(0); snap(name)
'''
b = src.index('snap("main")') + len('snap("main")')
looper = src[:b].replace("fails = 0", HELPER + "\nfails = 0", 1) + '''
def feed(n, k0, scale=1.0):
    for j in range(n):
        v = [drum((k0 + j * N + i) * 2) * scale for i in range(N)]
        block(v, [x * 0.9 for x in v])
k = 0
ev(1, REC_DOWN); feed(70, k); k += 70 * N; ev(1, REC_UP); block([0.0] * N)
ev(2, REC_DOWN); feed(70, k, 0.7); k += 70 * N; ev(2, REC_UP); block([0.0] * N)
ev(0, REC_DOWN); feed(40, k); k += 40 * N
idle_save()
draw(0); snap("looper_main")
for tab, name in ((1, "looper_fx"), (2, "looper_fx2"), (3, "looper_setup")):
    e.call("looper_page_goto", VIEW, tab); draw(0); snap(name)
'''
for name, code, test, env in (("m_samplr.py", samplr, "test_looper.py", "LOOPER_PNG"), ("m_looper.py", looper, "test_looper.py", "LOOPER_PNG"),
                              ("test_lira_screen.py", None, "test_lira_screen.py", "LIRA_PNG"), ("test_t2m_page.py", None, "test_t2m_page.py", None)):
    sb = C.make_sandbox("_shots", img, sets, test)
    if code:
        open(os.path.join(sb, name), "w").write(code)
    e = dict(os.environ)
    if env:
        e[env] = OUT
    r = subprocess.run([sys.executable, name], cwd=sb, env=e, capture_output=True, text=True, timeout=1800)
    print(name, "rc", r.returncode)
    assert r.returncode == 0, r.stderr[-600:]
    if name == "test_t2m_page.py":
        for f in ("hits", "detect", "out"):
            shutil.copy(os.path.join(sb, "out", "t2m_test", f"trig_{f}.png"), os.path.join(OUT, f"trig_{f}.png"))
for f in ("voices14", "sound", "chord", "mix", "env"):
    os.rename(os.path.join(OUT, f + ".png"), os.path.join(OUT, f"drone_{f}.png"))
print(sorted(os.listdir(OUT)))
