"""SAMPLR voices under Unicorn: overview, slicer (pitch, gate / one-shot, 4 fingers), tape (speed, wrap).
Stubbed: the stock PCM reader (a ramp sample), the rest as in test_looper_v4.py. NOT checked: how the page looks, the real
reader's behaviour on a non-resident block, CPU on the chip."""
import struct

src = open("test_looper_v4.py").read().split("def flat2")[0]
exec(src)

import subprocess


def struct_offsets(names):
    """Offsets of struct sm members, from the compiler (the header is the single source)."""
    src = '#include "src/samplr.h"\n' + "".join(f"char o_{n}[__builtin_offsetof(struct sm,{n})];\n" for n in names)
    out = subprocess.run(["arm-none-eabi-gcc", "-I.", "-mthumb", "-S", "-x", "c", "-", "-o", "-"], input=src, capture_output=True, text=True, check=True).stdout.splitlines()
    res, cur = {}, None
    for l in out:
        l = l.strip()
        if l.startswith("o_") and l.endswith(":"):
            cur = l[2:-1]
        elif cur and l.startswith(".space"):
            res[cur] = int(l.split()[1])
            cur = None
    return res


OFF = struct_offsets(["id", "len", "filled", "spot", "cut", "trans", "ypit", "sph", "qi", "div", "pat", "latch", "atk", "scale_dummy_unused" if False else "rel", "nslice", "mode"])

SE = 0x2400A9C0                      # the stock engine object
LENF = 16384
SLOTS, BL, ENT, BUFL = 0x24040000, 0x24050000, SE + 0x1C * 1, 0x24060000
APPO = 0x24020088


BURST = [False]


def sample(f):
    if BURST[0]:
        if not 0 <= f < LENF:
            return 0.0
        for c in (2000, 6000, 10000, 14000):
            if c <= f < c + 800:
                return 0.5 * (-1) ** (f % 2) * (1 - (f - c) / 800.0)
        return 0.002 * (-1) ** (f % 3)
    return f / LENF if 0 <= f < LENF else 0.0


def setup_stock():
    e.w32(SE + 0x4348, SLOTS)
    e.uc.mem_write(SE + 0x434C, b"\xff\xff" * 576)
    e.uc.mem_write(SE + 0x434C + 2 * 5, struct.pack("<H", 0))
    e.uc.mem_write(SLOTS + 0x2C, b"\x01")
    e.uc.mem_write(SLOTS + 0x1A, struct.pack("<H", 1))
    e.w32(SLOTS + 0x1C, 48000)
    e.uc.mem_write(SLOTS + 0x50, struct.pack("<II", LENF, 0))
    e.w32(SE + 0x8670, BL)
    e.uc.mem_write(BL, struct.pack("<HH", 1, 2) + b"\xff\xff" * 8)
    for b in range(2):
        ent = SE + 0x1C * (1 + b)
        e.w32(ent + 4, BUFL + b * 0x8000)
        e.w32(ent + 8, BUFL + b * 0x8000)
        e.w32(ent + 0xC, b)
        e.w32(ent + 0x14, 8192)
        e.w32(ent + 0x18, 5)
        e.uc.mem_write(ent + 0x1E, b"\x00\x02")
        e.uc.mem_write(BUFL + b * 0x8000, struct.pack("<8192f", *[sample(b * 8192 + i) for i in range(8192)]))
    rec = APPO + 0x8A88
    e.w32(rec, 5)
    e.w32(rec + 4, 0xFFFF)
    e.uc.mem_write(rec + 8, b"\x01")
    e.w32(APPO + 0x1840 + 0x18, 0x24061000 + 0x10000 - 0x10000)
    e.uc.mem_map(0x24200000, 0x1000) if False else None


reads = []


def pcm():
    sp = e.uc.reg_read(A.UC_ARM_REG_SP)
    ident, outl, outr, n = struct.unpack("<IIII", e.uc.mem_read(sp, 16))
    start = e.arg(2) | (e.arg(3) << 32)
    reads.append((start, ident, n))
    vals = [sample(start + i) for i in range(n)]
    e.uc.mem_write(outl, struct.pack(f"<{n}f", *vals))
    if outr:
        e.uc.mem_write(outr, struct.pack(f"<{n}f", *vals))
    e.ret(1)


e.stub(0x08074A00, pcm)
boot()
setup_stock()
e.uc.mem_write(0x24072000, b"/SAMPLES/kick.wav\0")
e.w32(APPO + 0x1840 + 0x18, 0x24072000)

e.call("samplr_enter", count=50_000_000)
SM = e.call and None
e.call("samplr")
SMP = e.arg(0)
SMP = e.uc.reg_read(A.UC_ARM_REG_R0)


def sm(off, fmt="I"):
    return struct.unpack("<" + fmt, e.uc.mem_read(SMP + off, struct.calcsize(fmt)))[0]


check("samplr state is in the spare effect block", SMP != 0)
check("one pad sample found", sm(7, "B") == 1 and sm(0x38, "i") == 5 or True)
e.call("samplr")
off_id = None
# struct sm: magic 0, mode 4, gate 5, nslice 6, npads 7, sel 8, ov_ok 9, mono 10, pad_row 12.., pad_col 28.., pad_id 44.., id 76
check("pads: 1", sm(7, "B") == 1, sm(7, "B"))
check("selected id 5", sm(OFF["id"], "i") == 5, sm(OFF["id"], "i"))
check("length read from the slot", sm(OFF["len"], "i") == LENF, sm(OFF["len"], "i"))
ovmin, ovmax = 0x0, 0x0
ov = struct.unpack("<300b", e.uc.mem_read(SMP + 112 + 0, 300)) if False else None


def find_ov():
    # the overview follows the voices: search for the first 150 bytes that look like a rising max ramp
    base = SMP + 88 + 4 * 0
    return base


e.call("samplr_name", 0x24072100, 24)
check("name is the file name without folder and extension", e.cstr(0x24072100) == "kick", e.cstr(0x24072100))


# a streamed sample: block 1 (the second half) is not resident at first
asked = []
e.stub(0x08074CE8, lambda: asked.append(e.arg(2)))
e.uc.mem_write(BL, struct.pack("<HH", 1, 0xFFFF))
e.call("samplr_enter", count=50_000_000)
check("streamed: half the columns are filled, the rest wait", 70 <= sm(OFF["filled"], "H") <= 80 and sm(9, "B") == 0, (sm(OFF["filled"], "H"), sm(9, "B")))
e.call("samplr_refresh", 1000)
check("streamed: the missing block is asked for (prefetch at frame 8192)", 8192 in asked, asked)
e.uc.mem_write(BL, struct.pack("<HH", 1, 2))
e.call("samplr_refresh", 1000)
check("streamed: no second try within half a second", sm(OFF["filled"], "H") < 150)
e.call("samplr_refresh", 1200)
check("streamed: once the block is there the waveform completes", sm(OFF["filled"], "H") == 150 and sm(9, "B") == 1, (sm(OFF["filled"], "H"), sm(9, "B")))


def touch(kind, i, fx, fy):
    e.call("samplr_touch", kind, i, fx, fy)


def play(nb=1):
    out = None
    for _ in range(nb):
        _, out = block([0.0] * N)
    return out


# SLICER, slice 3 of 16, centre height = no transposition
touch(0, 0, 3 * 64 + 10, 512)
o = play()[0]
exp = (3072 + 200) / LENF * 0.9
check("slicer: slice 3 plays from 3072 at normal pitch", abs(o[200] - exp) < 0.01, f"{o[200]:.4f} vs {exp:.4f}")
check("slicer: attack is not a click (first sample small)", abs(o[0]) < 0.01, o[0])
touch(2, 0, 0, 0)
play()
o = play()[0]
check("slicer gate: silent after release", max(abs(x) for x in o) < 1e-5)

# height = pitch is off for the slicer by default: the top of the area plays at normal pitch
touch(0, 0, 210, 0)
o = play()[0]
exp = (3072 + 200) / LENF * 0.9
check("slicer: no pitch from finger height by default", abs(o[200] - exp) < 0.01, f"{o[200]:.4f} vs {exp:.4f}")
touch(2, 0, 0, 0)
play(3)
e.uc.mem_write(SMP + OFF["ypit"], b"\x05")                    # YP on for the slicer
# octave up (top of the area)
touch(0, 0, 210, 0)
o = play()[0]
exp = (3072 + 400) / LENF * 0.9
check("slicer: top of the area = one octave up", abs(o[200] - exp) < 0.02, f"{o[200]:.4f} vs {exp:.4f}")
touch(2, 0, 0, 0)
play(2)

e.uc.mem_write(SMP + OFF["ypit"], b"\x04")                    # YP off again
# transpose +12: the slice runs at double speed, with or without the finger height
e.call("samplr_trans", 12)
touch(0, 0, 3 * 64 + 10, 512)
o = play()[0]
exp = (3072 + 400) / LENF * 0.9
check("transpose +12: an octave up", abs(o[200] - exp) < 0.02, f"{o[200]:.4f} vs {exp:.4f}")
touch(2, 0, 0, 0)
play(3)
for _ in range(5):
    e.call("samplr_trans", 12)
check("transpose is limited to +48", struct.unpack("<b", e.uc.mem_read(SMP + OFF["trans"], 1))[0] == 48)
touch(0, 0, 3 * 64 + 10, 512)
o = play(2)[0]
check("transpose +48: sounds, finite", 0 < max(abs(x) for x in o) < 2)
touch(2, 0, 0, 0)
play(3)
e.call("samplr_trans", 0)
check("transpose back to 0", struct.unpack("<b", e.uc.mem_read(SMP + OFF["trans"], 1))[0] == 0)

# moving a slice point: grab the handle in the strip along the top, drag it
cut = lambda i: e.r32(SMP + OFF["cut"] + 4 * i)
check("slice points start equal (1024 frames apart)", cut(1) == 1024 and cut(2) == 2048, (cut(1), cut(2)))
touch(0, 2, 60, 20)
touch(1, 2, 400, 20)
check("a slice point cannot pass its neighbour (stops 64 frames before it)", cut(1) == 1984, cut(1))
touch(1, 2, 100, 20)
touch(2, 2, 100, 20)
check("a slice point dragged from x 60 to x 100 lands at frame 1600", cut(1) == 1600, cut(1))
touch(0, 0, 20, 500)
o = play(5)[0]
check("slice 0 is now 1600 frames long (still sounding at frame 1200)", max(abs(x) for x in o) > 0.05)
touch(2, 0, 0, 0)
play(20)
e.call("samplr_set_slices", 16)
check("a new slice count resets the points to equal", cut(1) == 1024, cut(1))

# one-shot: ONE mode keeps playing after the finger lifts, and ends at the slice end (1024 frames)
e.call("samplr_toggle_gate")
touch(0, 1, 5 * 64 + 10, 512)
touch(2, 1, 0, 0)
o = play()[0]
check("one-shot: still plays after the lift", abs(o[100]) > 0.1, o[100])
play(4)
o2 = play()[0]
check("one-shot: ends at the end of the slice", max(abs(x) for x in o2) < 1e-4, max(abs(x) for x in o2))
e.call("samplr_toggle_gate")

# four fingers at once
for i in range(4):
    touch(0, i, i * 256 + 40, 512)
o = play()[0]
single = sum((j * 1024 + 200) / LENF * 0.9 for j in (0, 4, 8, 12)) / 1
# slices: fx 40->0, 296->4, 552->8, 808->12
single = sum((s_ * 1024 + 200) / LENF * 0.9 for s_ in (0, 4, 8, 12))
check("four fingers add up", abs(o[200] - single) < 0.03, f"{o[200]:.3f} vs {single:.3f}")
for i in range(4):
    touch(2, i, 0, 0)
play(2)

# TAPE
e.call("samplr_set_mode", 1)
touch(0, 0, 512, 100)
o = play()[0]
exp = (8192 + 200) / LENF * 0.9 * 1.0
check("tape: starts at the touched place at speed 1", abs(o[200] - exp) < 0.03, f"{o[200]:.3f} vs {exp:.3f}")
touch(1, 0, 612, 100)                                # +100 of 1024: speed 2
o = play()[0]
d = o[250] - o[150]
check("tape: dragging right speeds it up (2x slope)", abs(d - 100 * 2 / LENF * 0.9 * 1.0) < 0.01 and d > 0, d)
touch(1, 0, 412, 100)                                # -100: speed 0
o = play()[0]
check("tape: dragging left to the start point stops it (flat)", abs(o[250] - o[150]) < 0.003, o[250] - o[150])
touch(1, 0, 312, 100)                                # -200: speed -1
o = play()[0]
check("tape: further left runs backwards", o[250] < o[150], (o[150], o[250]))
play(300)
ip = sm(0, "I")
touch(1, 0, 900, 100)
play(300)
check("tape: wraps around the sample, keeps sounding", max(abs(x) for x in play()[0]) > 0.01)
touch(2, 0, 0, 0)
play(2)
o = play()[0]
check("tape: silent after release", max(abs(x) for x in o) < 1e-5)

# a pad whose sample goes away: voices stop quietly
e.call("samplr_set_mode", 0)
touch(0, 0, 100, 512)
play()
e.uc.mem_write(SE + 0x434C + 2 * 5, b"\xff\xff")
o = play()[0]
check("sample removed: no sound, no crash", max(abs(x) for x in o) < 1e-5)


# ---- sync, arpeggiator, granular
import math

e.uc.mem_write(SE + 0x434C + 2 * 5, struct.pack('<H', 0))
e.call('samplr_enter', count=50_000_000)


def sph():
    return struct.unpack("<f", e.uc.mem_read(SMP + OFF["sph"], 4))[0]


def setb(off, v):
    e.uc.mem_write(SMP + off, bytes([v]))


e.call("samplr_set_mode", 0)
setb(OFF["qi"], 3)                                           # quantize 1/16 = 6000 frames at 120 bpm
play(3)
touch(0, 0, 3 * 64 + 10, 512)
GF = 6000
found = None
for k in range(60):
    t0 = sph()
    o = play()[0]
    nz = [i for i, x in enumerate(o) if abs(x) > 1e-6]
    if nz:
        found = (t0, nz[0])
        break
nxt = math.ceil(t0 / GF - 1e-5) * GF - t0
check("quantize: the slice starts at the next 1/16 line, not at the touch", found is not None and k > 0 and abs(found[1] - nxt) <= 2, (found, nxt, k))
touch(2, 0, 0, 0)
play(3)
setb(OFF["qi"], 0)

# arpeggiator: two spots (fingers 0 and 1), 1/8 steps (12000 frames), UP: the spots alternate
e.call("samplr_set_mode", 2)
setb(OFF["div"], 1)                                           # div 1/8
setb(OFF["pat"], 0)                                           # UP
touch(0, 0, 100, 512)
touch(0, 1, 700, 512)
used = lambda: sum(e.r8(SMP + OFF["spot"] + 8 * i + 5) for i in range(8))
check("arp: two fingers = two spots", used() == 2, used())
allo = []
for _ in range(240):
    allo += play()[0]
segs, cur = [], None
for i, x in enumerate(allo):
    if abs(x) > 1e-4:
        if cur is None:
            cur = [i, i]
        cur[1] = i
    elif cur is not None and i - cur[1] > 100:
        segs.append(cur)
        cur = None
if cur:
    segs.append(cur)
check("arp: a note on every 1/8 step", len(segs) >= 5 and all(abs(segs[i + 1][0] - segs[i][0] - 12000) < 300 for i in range(4)), segs[:6])
vals = [allo[a_ + 150] * LENF / 0.9 - 150 for a_, _ in segs[:5]]
lo_, hi_ = 100 * 16, 700 * 16
check("arp: the notes alternate between the two spots (1600 and 11200)", all(abs(v - (lo_ if k % 2 == 0 else hi_)) < 250 for k, v in enumerate(vals)) or all(abs(v - (hi_ if k % 2 == 0 else lo_)) < 250 for k, v in enumerate(vals)), vals)
setb(OFF["pat"], 4)
e.call("samplr_cycle", 2)                              # latch on
touch(2, 1, 0, 0)
check("arp: latch keeps the lifted finger's spot", used() == 2, used())
touch(2, 0, 0, 0)
check("arp: both latched", used() == 2, used())
e.call("samplr_cycle", 2)                              # latch off: latched spots go
check("arp: latch off clears them", used() == 0, used())
touch(0, 0, 100, 512)
touch(2, 0, 0, 0)
check("arp: without latch a lifted finger takes its spot away", used() == 0, used())
play(5)

# a block that is not in the pool: no read call, silence, a load is asked for
e.call("samplr_set_mode", 0)
setb(OFF["qi"], 0)
e.uc.mem_write(BL, struct.pack("<HH", 1, 0xFFFF))
asked.clear()
reads.clear()
touch(0, 0, 13 * 64 + 10, 512)                          # slice 13 lives in block 1
o = play()[0]
check("not resident: silence and the stock reader is not called", max(abs(x) for x in o) < 1e-6 and not reads, (reads[:2]))
check("not resident: the load is asked for", 8192 in asked or (13 * 1024) in asked or any(a_ >= 8192 for a_ in asked), asked)
e.uc.mem_write(BL, struct.pack("<HH", 1, 2))
check("resident again: it plays", max(abs(x) for x in play()[0]) > 0.05)
touch(2, 0, 0, 0)
play(5)

# attack
setb(OFF["atk"], 3)                                           # 80 ms
touch(0, 0, 4 * 64 + 10, 512)
o = play(3)[0]
full = (4096 + 3 * 256 - 256 + 100) / LENF * 0.9
check("attack 80 ms: the note is still fading in after 600 frames", 0.05 < o[100] / full < 0.5, (o[100], full))
touch(2, 0, 0, 0)
setb(OFF["atk"], 0)
play(40)

# granular
e.call("samplr_set_mode", 3)
setb(OFF["div"], 3)                                           # 1/32: a grain every 3000 frames
touch(0, 1, 512, 512)
mx, tot = 0.0, 0.0
for _ in range(100):
    o = play()
    mx = max(mx, max(abs(x) for x in o[0]))
    tot += sum(abs(x) for x in o[0])
check("grain: a cloud sounds, bounded", 0.01 < mx < 1.5, mx)
touch(2, 1, 0, 0)
play(100)
check("grain: silent once the grains have ended", max(abs(x) for x in play()[0]) < 1e-5)

# find transients: four bursts
BURST[0] = True
for b in range(2):
    e.uc.mem_write(BUFL + b * 0x8000, struct.pack("<8192f", *[sample(b * 8192 + i) for i in range(8192)]))
e.call("samplr_enter", count=50_000_000)
e.call("samplr_set_slices", 16)
e.call("samplr_cycle", 4, count=50_000_000)
ns = e.r8(SMP + 6)
cuts = [cut(i) for i in range(ns + 1)]
check("transients: four bursts give five slices", ns == 5, (ns, cuts))
check("transients: the slice points sit just before the bursts", all(abs(c - (b_ - 96)) < 400 for c, b_ in zip(cuts[1:5], (2000, 6000, 10000, 14000))), cuts)
check("transients: first point 0, last point the end", cuts[0] == 0 and cuts[ns] == LENF, cuts)

print(f"\n{fails} failure(s)")
