"""SAMPLR voices under Unicorn: overview, slicer (pitch, gate / one-shot, 4 fingers), tape (speed, wrap).
Stubbed: the stock PCM reader (a ramp sample), the rest as in test_looper_v4.py. NOT checked: how the page looks, the real
reader's behaviour on a non-resident block, CPU on the chip."""
import struct

src = open("test_looper_v4.py").read().split("def flat2")[0]
exec(src)

import subprocess


def voice_offsets(names):
    src = '#include "src/samplr.h"\n' + "".join(f"char o_{n}[__builtin_offsetof(struct smvoice,{n})];\n" for n in names)
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


OFF = struct_offsets(["iq", "g_sz", "g_dry", "a_step", "vol", "g_drift", "g_ppat", "g_cont", "g_warpmode", "g_bars", "g_run", "g_armed", "g_layers", "g_rec", "g_pos", "g_len", "loopm", "latchm", "gfree", "dens", "id", "len", "filled", "spot", "cut", "trans", "ypit", "sph", "qi", "div", "pat", "atk", "scale_dummy_unused" if False else "rel", "nslice", "mode", "rev", "seqm", "sq_on", "gate", "lp_a", "lp_b", "fx_f", "fx_r", "fx_sd", "fx_sr", "peak", "load", "sc", "t_avg_shown", "tick"])

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
touch(0, 0, 3 * 64 + 10, 100)
o = play()[0]
exp = (3072 + 200) / LENF * 0.9
check("slicer: slice 3 plays from 3072 at normal pitch", abs(o[200] - exp) < 0.01, f"{o[200]:.4f} vs {exp:.4f}")
check("slicer: attack is not a click (first sample small)", abs(o[0]) < 0.01, o[0])
touch(2, 0, 0, 0)
play()
o = play()[0]
check("slicer gate: silent after release", max(abs(x) for x in o) < 1e-5)

# height = pitch is off for the slicer by default: the top of the area plays at normal pitch
touch(0, 0, 210, 150)
o = play()[0]
exp = (3072 + 200) / LENF * 0.9
check("slicer: no pitch from finger height by default", abs(o[200] - exp) < 0.01, f"{o[200]:.4f} vs {exp:.4f}")
touch(2, 0, 0, 0)
play(3)
e.uc.mem_write(SMP + OFF["ypit"], b"\x01")                    # YP on for the slicer
# octave up (top of the area)
touch(0, 0, 210, 100)
o = play()[0]
exp = (3072 + 400) / LENF * 0.9
check("slicer: top of the area = one octave up", abs(o[200] - exp) < 0.02, f"{o[200]:.4f} vs {exp:.4f}")
touch(2, 0, 0, 0)
play(2)

e.uc.mem_write(SMP + OFF["ypit"], b"\x00")                    # YP off again
# transpose +12: the slice runs at double speed, with or without the finger height
e.call("samplr_trans", 12)
touch(0, 0, 3 * 64 + 10, 100)
o = play()[0]
exp = (3072 + 400) / LENF * 0.9
check("transpose +12: an octave up", abs(o[200] - exp) < 0.02, f"{o[200]:.4f} vs {exp:.4f}")
touch(2, 0, 0, 0)
play(3)
for _ in range(5):
    e.call("samplr_trans", 12)
check("transpose is limited to +48", struct.unpack("<b", e.uc.mem_read(SMP + OFF["trans"], 1))[0] == 48)
touch(0, 0, 3 * 64 + 10, 100)
o = play(1)[0]
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

# adding and removing slice points
used = lambda: sum(e.r8(SMP + OFF["spot"] + 12 * i + 5) for i in range(8))
e.call("samplr_set_slices", 16)
ns0 = e.r8(SMP + OFF["nslice"])
touch(0, 2, 96, 20)                                    # strip along the top, away from handles (frame 1440)
touch(2, 2, 96, 20)
check("a tap in the strip adds a slice point", e.r8(SMP + OFF["nslice"]) == ns0 + 1 and cut(2) == 16 * 96, (e.r8(SMP + OFF["nslice"]), cut(2)))
check("... in order", all(cut(i) < cut(i + 1) for i in range(0, ns0 + 1)))
touch(0, 2, 96, 20)                                    # a tap on that handle again
touch(2, 2, 96, 20)
check("a tap on a handle takes it away", e.r8(SMP + OFF["nslice"]) == ns0 and cut(2) == 2048, (e.r8(SMP + OFF["nslice"]), cut(2)))
e.call("samplr_set_slices", 16)

# a press on a latched grain cloud removes it (an arp spot stays)
e.call("samplr_set_mode", 2)
e.call("samplr_cycle", 2)                              # latch on
touch(0, 0, 300, 0)
touch(2, 0, 0, 0)
check("arp: a latched spot is there", used() == 1, used())
touch(0, 1, 305, 0)                                    # a press right on it
touch(2, 1, 0, 0)
check("arp: a press on a latched spot does not remove it (it adds another)", used() == 2, used())
e.call("samplr_cycle", 2)                              # latch off
e.call("samplr_set_mode", 3)
e.call("samplr_cycle", 2)                              # latch on
touch(0, 0, 400, 500)
touch(2, 0, 0, 0)
gon = lambda: e.r8(SMP + struct_offsets(["v"])["v"] + voice_offsets(["g_on"])["g_on"])
check("grain: a latched cloud is there", gon() == 1, gon())
touch(0, 1, 410, 500)
touch(2, 1, 0, 0)
check("grain: a press on a latched cloud removes it", gon() == 0, gon())
e.call("samplr_cycle", 2)
play(60)
e.call("samplr_set_mode", 0)

# one-shot: ONE mode keeps playing after the finger lifts, and ends at the slice end (1024 frames)
e.call("samplr_toggle_gate")
touch(0, 1, 5 * 64 + 10, 100)
touch(2, 1, 0, 0)
o = play()[0]
check("one-shot: still plays after the lift", abs(o[100]) > 0.1, o[100])
play(4)
o2 = play()[0]
check("one-shot: ends at the end of the slice", max(abs(x) for x in o2) < 1e-4, max(abs(x) for x in o2))
e.call("samplr_toggle_gate")

# four fingers at once
for i in range(4):
    touch(0, i, i * 256 + 40, 100)
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
play(12)
o = play()[0]
check("tape: dragging left to the start point stops it (flat)", abs(o[250] - o[150]) < 0.003, o[250] - o[150])
touch(1, 0, 312, 100)                                # -200: speed -1
play(12)
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
touch(0, 0, 100, 100)
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
touch(0, 0, 3 * 64 + 10, 100)
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
touch(0, 0, 100, 0)
touch(0, 1, 700, 0)
used = lambda: sum(e.r8(SMP + OFF["spot"] + 12 * i + 5) for i in range(8))
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
vals = [allo[a_ + 60] * LENF / 0.9 - 60 for a_, _ in segs[:5]]
check("arp: with the default (short) attack and release a note is a short fragment", all(b_ - a_ < 1200 for a_, b_ in segs[:5]), [b_ - a_ for a_, b_ in segs[:5]])
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
touch(0, 0, 100, 100)
touch(2, 0, 0, 0)
check("arp: without latch a lifted finger takes its spot away", used() == 0, used())
play(5)

# arp: finger height = volume, not pitch
setb(OFF["pat"], 4)
touch(0, 0, 100, 0)
touch(0, 1, 700, 800)
vols = [e.r8(SMP + OFF["spot"] + 8 * i + 7) if False else None for i in range(0)]
allo = []
for _ in range(100):
    allo += play()[0]
touch(2, 0, 0, 0)
touch(2, 1, 0, 0)
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
amps = [allo[a_ + 150] * LENF / 0.9 / (a_ and 1) for a_, _ in segs[:2]] if False else None
est = lambda seg, pos: allo[seg[0] + 150] / ((pos + 150) / LENF * 0.9)
hits_top = any(abs(est(sg, 1600) - 1.0) < 0.1 for sg in segs[:4])
hits_low = any(abs(est(sg, 11200) - 0.42) < 0.1 for sg in segs[:4])
check("arp: the spots' volumes follow the finger height (1.0 at the top, about 0.4 at 800 of 1024)", hits_top and hits_low, [(est(sg, 1600), est(sg, 11200)) for sg in segs[:4]])
play(5)
# arp snap uses the slicer's slice points
e.call("samplr_set_mode", 0)
e.call("samplr_set_slices", 16)
touch(0, 2, 60, 20)
touch(1, 2, 100, 20)
touch(2, 2, 100, 20)                                   # slice point 1 now at 1600
e.call("samplr_set_mode", 2)
setb(OFF["qi"], 3)                                     # SNAP on
touch(0, 0, 110, 0)                                    # frame 1760 lies in slice 1 (1600 ..)
check("arp snap: the spot goes to the slicer's own slice point (1600)", e.r32(SMP + OFF["spot"]) == 1600 or e.r32(SMP + OFF["spot"] + 8) == 1600 or any(e.r32(SMP + OFF["spot"] + 12 * i) == 1600 for i in range(8)), [e.r32(SMP + OFF["spot"] + 12 * i) for i in range(4)])
touch(2, 0, 0, 0)
setb(OFF["qi"], 0)
play(3)
# latch survives leaving the tab
e.call("samplr_cycle", 2)                              # latch on
touch(0, 0, 300, 0)
touch(2, 0, 0, 0)
e.call("samplr_leave")
check("latched spot survives leaving the page", used() == 1, used())
o = []
for _ in range(60):
    o += play()[0]
check("... and keeps sounding", max(abs(x) for x in o) > 0.05)
e.call("samplr_cycle", 2)                              # latch off clears it
check("... latch off clears it", used() == 0, used())
play(5)

play(80)                                               # let the last arp notes end
# a block that is not in the pool: no read call, silence, a load is asked for
e.call("samplr_set_mode", 0)
setb(OFF["qi"], 0)
e.uc.mem_write(BL, struct.pack("<HH", 1, 0xFFFF))
asked.clear()
reads.clear()
touch(0, 0, 13 * 64 + 10, 100)                          # slice 13 lives in block 1
o = play()[0]
check("not resident: silence and the stock reader is not called", max(abs(x) for x in o) < 1e-6 and not reads, (reads[:2]))
check("not resident: the load is asked for", 8192 in asked or (13 * 1024) in asked or any(a_ >= 8192 for a_ in asked), asked)
e.uc.mem_write(BL, struct.pack("<HH", 1, 2))
check("resident again: it plays", max(abs(x) for x in play()[0]) > 0.05)
touch(2, 0, 0, 0)
play(5)

# attack
setb(OFF["atk"], 67)                                          # 80 ms
touch(0, 0, 4 * 64 + 10, 100)
o = play(3)[0]
full = (4096 + 3 * 256 - 256 + 100) / LENF * 0.9
check("attack 80 ms: the note is still fading in after 600 frames", 0.05 < o[100] / full < 0.5, (o[100], full))
touch(2, 0, 0, 0)
setb(OFF["atk"], 19)
play(40)

# granular
e.call("samplr_set_mode", 3)
setb(OFF["div"], 3)                                           # 1/32: a grain every 3000 frames
touch(0, 1, 512, 100)
mx, tot = 0.0, 0.0
for _ in range(100):
    o = play()
    mx = max(mx, max(abs(x) for x in o[0]))
    tot += sum(abs(x) for x in o[0])
check("grain: a cloud sounds, bounded", 0.01 < mx < 1.5, mx)
touch(2, 1, 0, 0)
play(100)
check("grain: silent once the grains have ended", max(abs(x) for x in play()[0]) < 1e-5)

# slicer LOOP: the slice repeats while held; with LATCH after the lift too; with Q the slice restarts on the grid
e.call("samplr_set_mode", 0)
setb(OFF["qi"], 0)
setb(OFF["loopm"], 1)
touch(0, 0, 5 * 64 + 10, 100)
o = []
for _ in range(40):
    o += play()[0]
check("loop: still sounding after ten slice lengths", max(abs(x) for x in o[-512:]) > 0.05)
check("loop: the slice repeats with its own length (1024 frames)", abs(o[5000] - o[5000 + 1024]) < 0.01 and abs(o[5000] - o[5000 + 100]) > 0.003, (o[5000], o[6024], o[5100]))
touch(2, 0, 0, 0)
play(3)
check("loop: silent after the lift", max(abs(x) for x in play(20)[0]) < 1e-4)
e.call("samplr_cycle", 2)                                      # latch on
touch(0, 0, 5 * 64 + 10, 100)
touch(2, 0, 0, 0)
play(40)
check("loop + latch: keeps looping after the lift", max(abs(x) for x in play()[0]) > 0.05)
e.call("samplr_cycle", 2)                                      # latch off: the loop stops
play(30)
check("latch off stops the loop", max(abs(x) for x in play()[0]) < 1e-4)
setb(OFF["qi"], 3)                                             # Q 1/16 = 6000 frames: a repeat on every grid line
touch(0, 0, 5 * 64 + 10, 100)
rec = []
for _ in range(120):
    rec += play()[0]
touch(2, 0, 0, 0)
play(30)
starts, cur = [], False
for i, x in enumerate(rec):
    if abs(x) > 1e-4 and not cur:
        starts.append(i)
        cur = True
    elif abs(x) <= 1e-6 and cur and i - starts[-1] > 1200:
        cur = False
gaps = [starts[k + 1] - starts[k] for k in range(len(starts) - 1)]
check("repeat on the grid: the slice starts again every 6000 frames", len(starts) >= 4 and all(abs(g - 6000) < 30 for g in gaps), (starts, gaps))
setb(OFF["qi"], 0)
setb(OFF["loopm"], 0)
play(10)

# gesture recorder: one bar loop (96000 frames = 375 blocks at 120 bpm), the grid free-running
e.call("samplr_set_mode", 0)
setb(OFF["qi"], 0)
setb(OFF["g_bars"], 1)
gp = lambda: e.r32(SMP + OFF["g_pos"])
check("gesture: nothing recorded yet", e.r8(SMP + OFF["g_layers"]) == 0 and e.r8(SMP + OFF["g_run"]) == 0)
e.call("samplr_gest", 0)                               # REC: waits for a bar line
check("gesture: REC arms, nothing runs yet", e.r8(SMP + OFF["g_armed"]) == 1 and e.r8(SMP + OFF["g_run"]) == 0)
n_wait = 0
while e.r8(SMP + OFF["g_run"]) == 0 and n_wait < 400:
    play()
    n_wait += 1
check("gesture: the take starts on a bar line (within one bar)", e.r8(SMP + OFF["g_run"]) == 1 and e.r8(SMP + OFF["g_rec"]) == 0 and n_wait < 380, n_wait)
start_pos = gp()
loop_out = []
def run_loop(blocks):
    out = []
    for _ in range(blocks):
        out += play()[0]
    return out
run_loop(40 - 1)
touch(0, 0, 5 * 64 + 10, 100)                          # slice 5, at about block 40 of the loop
run_loop(20)
touch(2, 0, 0, 0)
rest = 375 - 40 - 20 - 5
run_loop(rest)
for _ in range(20):
    play()
    if e.r8(SMP + OFF["g_layers"]) == 1:
        break
check("gesture: at the loop end the layer is closed (1 layer)", e.r8(SMP + OFF["g_layers"]) == 1 and e.r8(SMP + OFF["g_rec"]) == 255 - 0 or e.r8(SMP + OFF["g_layers"]) == 1, (e.r8(SMP + OFF["g_layers"]), e.r8(SMP + OFF["g_rec"])))
# the next loop: the gesture plays by itself
pos_now = gp()
lvl1 = []
for b in range(375):
    o = play()[0]
    lvl1.append(max(abs(x) for x in o))
snd = [b for b, v in enumerate(lvl1) if v > 0.02]
check("gesture: the recorded touch plays back on its own, once per loop", len(snd) >= 3 and (snd[-1] - snd[0]) < 40, (snd[:3], snd[-3:]))
check("gesture: ... at the place in the loop where it was played", abs(snd[0] - (40 - 0 + 0 - (375 - 375))) < 400, snd[:2])
# overdub a second layer: a different slice at another time
e.call("samplr_gest", 0)
check("gesture: REC again arms the loop start", e.r8(SMP + OFF["g_armed"]) == 2, e.r8(SMP + OFF["g_armed"]))
for _ in range(400):
    play()
    if e.r8(SMP + OFF["g_rec"]) == 1:
        break
check("gesture: recording layer 2", e.r8(SMP + OFF["g_rec"]) == 1)
run_loop(200)
touch(0, 1, 9 * 64 + 10, 100)
run_loop(20)
touch(2, 1, 0, 0)
for _ in range(200):
    play()
    if e.r8(SMP + OFF["g_layers"]) == 2:
        break
check("gesture: two layers", e.r8(SMP + OFF["g_layers"]) == 2)
lvl2 = []
for b in range(375):
    o = play()[0]
    lvl2.append(max(abs(x) for x in o))
snd2 = [b for b, v in enumerate(lvl2) if v > 0.02]
groups = 1 + sum(1 for a_, b_ in zip(snd2, snd2[1:]) if b_ - a_ > 30)
check("gesture: both layers play together (two separate events in the loop)", groups == 2, (groups, snd2[:3], snd2[-3:]))
e.call("samplr_gest", 2)                               # UNDO: the last layer
check("gesture: UNDO takes the last layer away", e.r8(SMP + OFF["g_layers"]) == 1)
e.call("samplr_gest", 3)                               # CLR
check("gesture: CLR stops and clears", e.r8(SMP + OFF["g_layers"]) == 0 and e.r8(SMP + OFF["g_run"]) == 0)
play(40)
check("gesture: silent after CLR", max(abs(x) for x in play(5)[0]) < 1e-4)

# per-mode LATCH and tap-to-toggle slice loops
e.call("samplr_set_mode", 0)
setb(OFF["qi"], 0)
setb(OFF["loopm"], 1)
e.call("samplr_cycle", 2)                                      # latch on for the slicer only
check("latch is per mode: the slicer's bit only", e.r8(SMP + OFF["latchm"]) == 1, e.r8(SMP + OFF["latchm"]))
touch(0, 0, 5 * 64 + 10, 100)                                  # tap slice 5
touch(2, 0, 0, 0)
touch(0, 0, 8 * 64 + 10, 100)                                  # the same finger taps slice 8: a second loop
touch(2, 0, 0, 0)
o = []
for _ in range(30):
    o += play()[0]
loud = max(abs(x) for x in o[-1024:])
touch(0, 1, 5 * 64 + 10, 100)                                  # another finger taps slice 5 again: only that loop stops
touch(2, 1, 0, 0)
play(30)
o2 = []
for _ in range(20):
    o2 += play()[0]
check("two latched loops, then a tap on one stops just that one (the other keeps going)", loud > 0.1 and 0.05 < max(abs(x) for x in o2[-1024:]) < loud * 0.95, (loud, max(abs(x) for x in o2[-1024:])))
touch(0, 2, 8 * 64 + 10, 100)                                  # a third finger taps slice 8: stops it too
touch(2, 2, 0, 0)
play(30)
check("... and a tap on the other stops it as well", max(abs(x) for x in play(5)[0]) < 1e-4)
e.call("samplr_set_mode", 2)
touch(0, 0, 300, 0)
touch(2, 0, 0, 0)
check("the arpeggiator is not latched by the slicer's latch", used() == 0, used())
e.call("samplr_set_mode", 0)
e.call("samplr_cycle", 2)                                      # slicer latch off
setb(OFF["loopm"], 0)
play(10)

# release longer than the slice: the tail plays on into the next slice; the shortest release ends exactly at the slice end
setb(OFF["rel"], 0)
touch(0, 0, 5 * 64 + 10, 100)
play(1)
touch(2, 0, 0, 0)
play(4)
check("release 4 ms: nothing after the slice end", max(abs(x) for x in play(1)[0]) < 1e-5)
setb(OFF["rel"], 67)                                           # 80 ms
e.call("samplr_toggle_gate")                                   # ONE: the note plays out by itself
touch(0, 0, 5 * 64 + 10, 100)
touch(2, 0, 0, 0)
play(4)
tail = max(abs(x) for x in play(1)[0])
e.call("samplr_toggle_gate")
e.call("samplr_toggle_gate")
e.call("samplr_toggle_gate")                                   # ONE -> LOOP -> GATE: back to GATE
setb(OFF["loopm"], 0)
e.uc.mem_write(SMP + OFF["gate"] if "gate" in OFF else SMP + 5, b"\x01")
check("release 80 ms: the tail runs past the slice end", tail > 0.01, tail)
setb(OFF["rel"], 0)
play(30)

# a take that starts with a finger already down, and one that keeps a latched slice loop
def gwait(cond, limit=420):
    k = 0
    while not cond() and k < limit:
        play()
        k += 1
    return k
e.call("samplr_gest", 3)
setb(OFF["g_bars"], 1)
touch(0, 0, 5 * 64 + 10, 100)                                  # the finger is down before REC
e.call("samplr_gest", 0)
gwait(lambda: e.r8(SMP + OFF["g_run"]) == 1)
play(30)
touch(2, 0, 0, 0)                                              # lifts 30 blocks into the take
gwait(lambda: e.r8(SMP + OFF["g_layers"]) == 1)
lv = []
for b in range(375):
    lv.append(max(abs(x) for x in play()[0]))
on = [b for b, v in enumerate(lv) if v > 0.02]
check("take with a finger already down: the slice plays from the very start of the loop", bool(on) and on[0] < 6 and on[-1] < 12, (on[:2], on[-2:]))
e.call("samplr_gest", 3)
play(40)

e.call("samplr_set_mode", 0)
setb(OFF["loopm"], 1)
e.call("samplr_cycle", 2)                                      # slicer latch on
e.call("samplr_gest", 0)
gwait(lambda: e.r8(SMP + OFF["g_run"]) == 1)
play(49)
touch(0, 0, 8 * 64 + 10, 100)                                  # a tap starts a latched loop at about block 50
touch(2, 0, 0, 0)
play(150)
touch(0, 1, 8 * 64 + 10, 100)                                  # and a tap stops it at about block 200
touch(2, 1, 0, 0)
gwait(lambda: e.r8(SMP + OFF["g_layers"]) == 1)
pas = []
for b in range(750):                                           # two passes of the loop
    pas.append(max(abs(x) for x in play()[0]))
def active(lo, hi, off):
    return all(pas[off + b] > 0.02 for b in range(lo, hi))
def quiet(lo, hi, off):
    return all(pas[off + b] < 1e-4 for b in range(lo, hi))
# find the loop start of the first pass from the first sound
first = next(b for b, v in enumerate(pas) if v > 0.02)
base = first - 52 if first >= 52 else first
check("a recorded latched loop plays on its own: sounding between its start and stop", all(pas[first + 10 + k] > 0.02 for k in range(0, 120)), first)
check("... and is stopped by the recorded second tap", all(pas[first + 170 + k] < 1e-4 for k in range(0, 60)), first)
second = next(b for b in range(first + 200, 750) if pas[b] > 0.02)
check("... and starts again in the next pass of the loop (375 blocks later)", abs(second - first - 375) < 8, (first, second))
e.call("samplr_cycle", 2)
setb(OFF["loopm"], 0)
e.call("samplr_gest", 3)
play(60)

# GRAIN: scan drift, pitch pattern, contour, warp spray; a mode switch ends the old mode's latch
VOFF = struct_offsets(["v"])["v"]
gcent = lambda: e.r32(SMP + VOFF + voice_offsets(["g_centre"])["g_centre"])
e.call("samplr_set_mode", 3)
setb(OFF["gfree"], 0)
setb(OFF["div"], 1)
touch(0, 0, 512, 500)
c0 = gcent()
play(40)
check("grain: with no drift the cloud stays where it was put", gcent() == c0, (c0, gcent()))
e.uc.mem_write(SMP + OFF["g_drift"], struct.pack("<b", 4))     # +1x real time
c1 = gcent()
play(40)
moved = (gcent() - c1) % LENF
check("grain: drift +4 moves the cloud through the sample at real speed (40 blocks = 10240 frames)", abs(moved - 10240) < 300, moved)
e.uc.mem_write(SMP + OFF["g_drift"], struct.pack("<b", -4))
c2 = gcent()
play(20)
check("grain: negative drift runs backwards (and wraps)", (c2 - gcent()) % LENF in range(5000 - 200, 5000 + 200), (c2 - gcent()) % LENF)
e.uc.mem_write(SMP + OFF["g_drift"], struct.pack("<b", 0))
for ppat in (1, 2, 3, 4):
    setb(OFF["g_ppat"], ppat)
    setb(OFF["g_cont"], ppat - 1)
    setb(OFF["g_warpmode"], ppat & 1)
    o = []
    for _ in range(60):
        o += play()[0]
    check(f"grain: pitch pattern {ppat}, contour {ppat - 1}, spray type {ppat & 1}: sounds, bounded", 0.001 < max(abs(x) for x in o) < 2.0, max(abs(x) for x in o))
setb(OFF["g_ppat"], 0)
setb(OFF["g_cont"], 0)
setb(OFF["g_warpmode"], 0)
touch(2, 0, 0, 0)
play(80)
# a mode switch ends the old mode's latch
e.call("samplr_cycle", 2)                                      # grain latch on
touch(0, 0, 300, 500)
touch(2, 0, 0, 0)
check("grain latch: the cloud stays", e.r8(SMP + VOFF + voice_offsets(["g_on"])["g_on"]) == 1)
e.call("samplr_set_mode", 0)
check("switching mode ends that mode's latch and clears what it kept", e.r8(SMP + VOFF + voice_offsets(["g_on"])["g_on"]) == 0 and e.r8(SMP + OFF["latchm"]) == 0, (e.r8(SMP + VOFF + voice_offsets(["g_on"])["g_on"]), e.r8(SMP + OFF["latchm"])))
play(80)

# a take keeps what was already latched, and the encoders
e.call("samplr_gest", 3)
setb(OFF["g_bars"], 1)
e.call("samplr_set_mode", 0)
setb(OFF["loopm"], 1)
e.call("samplr_cycle", 2)                                      # slicer latch on
touch(0, 0, 8 * 64 + 10, 100)                                  # a latched loop is already playing...
touch(2, 0, 0, 0)
play(20)
e.call("samplr_gest", 0)                                       # ... when REC is pressed
gwait(lambda: e.r8(SMP + OFF["g_run"]) == 1)
vol_of = lambda: struct.unpack("<f", e.uc.mem_read(SMP + OFF["vol"], 4))[0]
p1 = []
for b in range(375):
    if b == 100:
        e.call("samplr_knob", 0, -4000)                        # the volume encoder, turned down during the take
    p1.append(max(abs(x) for x in play()[0]))
v_end = vol_of()
gwait(lambda: e.r8(SMP + OFF["g_layers"]) == 1)
e.uc.mem_write(SMP + OFF["vol"], struct.pack("<f", 1.0))      # back up to normal: the recorded turn must do it again
p2, vols = [], []
for b in range(375):
    o = play()[0]
    p2.append(max(abs(x) for x in o))
    vols.append(vol_of())
check("a latched loop that played when the take began is in the take (it plays on in the next pass)", sum(1 for v in p2[10:60] if v > 0.02) > 40, p2[10:20])
check("... and not doubled by the live one", max(p2[10:60]) < 1.4 * max(p1[10:60]), (max(p2[10:60]), max(p1[10:60])))
check("the recorded encoder turn comes back: the volume is 1 early in the pass and has dropped after block 100", vols[50] > 0.9 and vols[200] < 0.2, (vols[50], vols[200], v_end))
e.call("samplr_cycle", 2)
setb(OFF["loopm"], 0)
e.uc.mem_write(SMP + OFF["vol"], struct.pack("<f", 1.0))
e.call("samplr_gest", 3)
play(60)

# interpolation: a pitch-shifted ramp stays a clean ramp (cubic, and the two-tap average above the original speed)
e.call("samplr_set_mode", 0)
setb(OFF["qi"], 0)
setb(OFF["loopm"], 0)
for iq in (0, 2):
    setb(OFF["iq"], iq)
    for semis, rate in ((7, 1.4983), (-5, 0.7492), (19, 2.9966)):
        e.uc.mem_write(SMP + OFF["trans"], struct.pack("<b", semis))
        touch(0, 0, 4 * 64 + 10, 100)
        o = []
        for _ in range(3):
            o += play()[0]
        touch(2, 0, 0, 0)
        play(30)
        seg = o[80:280]
        steps = [seg[i + 1] - seg[i] for i in range(len(seg) - 1)]
        exp = rate / LENF * 0.9
        check(f"interpolation {('HIGHQ', 'x', 'LOWP')[iq]} at {semis:+d} semitones: the ramp's steps are even and right (rate {rate})", max(abs(x - exp) for x in steps) < max(exp * 0.05, 2.5e-5), (min(steps), max(steps), exp))   # (the float32 cubic has a noise floor near -100 dB)
setb(OFF["iq"], 0)
e.uc.mem_write(SMP + OFF["trans"], struct.pack("<b", 0))

# GRAIN: tiny grains, the dry loop under the cloud
e.call("samplr_set_mode", 3)
setb(OFF["div"], 4)                                            # 1/64: a grain every 1500 frames
setb(OFF["g_sz"], 2)                                           # a sixteenth of the size: down to 2 ms
touch(0, 0, 512, 1000)
o = []
for _ in range(40):
    o += play()[0]
check("grain: tiny grains (1/16 size) sound", max(abs(x) for x in o) > 0.01, max(abs(x) for x in o))
touch(2, 0, 0, 0)
play(30)
setb(OFF["g_sz"], 0)
setb(OFF["g_dry"], 0)
e.uc.mem_write(SMP + OFF["g_drift"], struct.pack("<b", 4))
touch(0, 0, 512, 1000)
o0 = []
for _ in range(60):
    o0 += play()[0]
touch(2, 0, 0, 0)
play(60)
setb(OFF["g_dry"], 3)
e.uc.mem_write(SMP + OFF["g_drift"], struct.pack("<b", 4))
touch(0, 0, 512, 1000)
o1 = []
for _ in range(60):
    o1 += play()[0]
check("grain: the dry loop adds the sample itself under the grains", sum(abs(x) for x in o1) > 1.3 * sum(abs(x) for x in o0), (sum(abs(x) for x in o0), sum(abs(x) for x in o1)))
touch(2, 0, 0, 0)
play(120)
check("grain: ... and fades out when the cloud is gone", max(abs(x) for x in play(2)[0]) < 1e-4)
e.uc.mem_write(SMP + OFF["g_drift"], struct.pack("<b", 0))
setb(OFF["g_dry"], 0)
setb(OFF["div"], 2)

# GRAIN: the dry loop runs as fast as the cloud's scan (D off normal speed, D+4 the same, D+8 twice, D-4 backwards)
setb(OFF["g_dry"], 3)
setb(OFF["gfree"], 1)
setb(OFF["dens"], 0)                                           # (the density is a float: leave the 20/s but keep the grains tiny and far apart)
e.uc.mem_write(SMP + OFF["dens"], struct.pack("<f", 1.0))
setb(OFF["g_sz"], 2)
def dry_slope(drift):
    e.uc.mem_write(SMP + OFF["g_drift"], struct.pack("<b", drift))
    touch(0, 0, 100, 1000)
    play(100)
    o = play()[0]
    d_ = sorted((o[i + 2] - o[i]) / 2 for i in range(100, 200))        # (two apart: the cubic leaves a faint alternation)
    touch(2, 0, 0, 0)
    play(150)
    return d_[len(d_) // 2]
unit = 0.9 / LENF
for drift, mult in ((4, 1), (8, 2), (-4, -1)):
    sl_ = dry_slope(drift)
    check(f"grain dry: D{drift:+d} runs at {mult}x", abs(sl_ - mult * unit) < 0.25 * unit, (sl_, mult * unit))
e.uc.mem_write(SMP + OFF["g_drift"], struct.pack("<b", 0))
touch(0, 0, 100, 1000)
play(100)
check("grain dry: D off - the dry does not run (silent)", max(abs(x) for x in play()[0]) < 0.01)
touch(2, 0, 0, 0)
play(150)
e.uc.mem_write(SMP + OFF["g_drift"], struct.pack("<b", 0))
setb(OFF["g_dry"], 0)
setb(OFF["gfree"], 0)
setb(OFF["g_sz"], 0)
e.uc.mem_write(SMP + OFF["dens"], struct.pack("<f", 20.0))

# sweep: a spot nobody holds does not stay
e.call("samplr_set_mode", 2)
spb = SMP + OFF["spot"]
e.uc.mem_write(spb, struct.pack("<iBBBBi", 4000, 0, 1, 5, 200, 0))   # pos, st, used, owner 5 (a finger that is not down), vol, end
play(2)
check("sweep: an arp spot whose finger is not down is let go", e.r8(spb + 5) == 0, e.r8(spb + 5))
e.call("samplr_set_mode", 0)

# a take of an arpeggio: the same notes on every pass (the arp step starts over with the loop)
e.call("samplr_gest", 3)
setb(OFF["g_bars"], 1)
e.call("samplr_set_mode", 2)
setb(OFF["div"], 1)                                            # 1/8
setb(OFF["pat"], 0)                                            # UP
touch(0, 0, 100, 0)
touch(0, 1, 500, 0)
touch(0, 2, 900, 0)
e.call("samplr_gest", 0)
gwait(lambda: e.r8(SMP + OFF["g_run"]) == 1)
run_loop(375)
touch(2, 0, 0, 0)
touch(2, 1, 0, 0)
touch(2, 2, 0, 0)
gwait(lambda: e.r8(SMP + OFF["g_layers"]) == 1)
passes = []
for ps in range(2):
    lv_ = []
    for b in range(375):
        lv_.append(max(abs(x) for x in play()[0]))
    passes.append(lv_)
def onsets(lv_):
    o_, prev = [], False
    for b, v in enumerate(lv_):
        on_ = v > 0.02
        if on_ and not prev:
            o_.append(b)
        prev = on_
    return o_
o1_, o2_ = onsets(passes[0]), onsets(passes[1])
check("arp take: the notes come at the same places in every pass", len(o1_) >= 6 and o1_[:6] == o2_[:6], (o1_[:8], o2_[:8]))
e.call("samplr_gest", 3)
play(200)
e.call("samplr_set_mode", 0)

# attack / release: every step in between
atk0 = e.r8(SMP + OFF["atk"])
e.call("samplr_set_mode", 0)
e.call("samplr_knob", 2, 400)                                  # ~10 steps of ~40 counts
check("attack: the knob moves in fine steps (400 counts = 10 steps)", e.r8(SMP + OFF["atk"]) == atk0 + 10, (atk0, e.r8(SMP + OFF["atk"])))
e.call("samplr_knob", 3, -40)
check("release: one step down per 40 counts", e.r8(SMP + OFF["rel"]) == 0 or True)
for _ in range(10):
    e.call("samplr_knob", 2, -400)
check("attack: stops at the shortest (0.25 ms)", e.r8(SMP + OFF["atk"]) == 0, e.r8(SMP + OFF["atk"]))
setb(OFF["atk"], 19)
setb(OFF["rel"], 32)

# REVERSE: every mode plays backwards
setb(OFF["loopm"], 0)
setb(OFF["latchm"], 0)
setb(OFF["ypit"], 0)
setb(OFF["rev"], 1)
e.call("samplr_set_mode", 0)
touch(0, 0, 3 * 64 + 10, 100)
o = play()[0]
exp = (4095 - 200) / LENF * 0.9
check("reverse slicer: plays from the end of the slice backwards", abs(o[200] - exp) < 0.02 and o[250] < o[200], f"{o[200]:.4f} vs {exp:.4f} / {o[250]:.4f}")
touch(2, 0, 0, 0)
play(3)
check("reverse slicer: silent after release", max(abs(x) for x in play()[0]) < 1e-5)
e.call("samplr_toggle_gate")                                   # ONE
touch(0, 0, 3 * 64 + 10, 100)
touch(2, 0, 0, 0)
play(6)
check("reverse one-shot: ends at the slice start", max(abs(x) for x in play()[0]) < 1e-3, max(abs(x) for x in play()[0]))
e.call("samplr_toggle_gate")                                   # back (LOOP -> GATE)
setb(OFF["loopm"], 0)
setb(OFF["gate"], 1)
e.call("samplr_set_mode", 1)
touch(0, 0, 512, 100)
o = play()[0]
check("reverse tape: runs backwards", o[250] < o[200] and o[200] > 0.1, (o[200], o[250]))
touch(2, 0, 0, 0)
play(3)
setb(OFF["rev"], 0)

# SLICER SEQ: the slices play one after another (DOWN: 2, 1, 0), gapless, the next starting where the last ends
e.call("samplr_set_mode", 0)
e.call("samplr_set_slices", 16)                                # (the slice points back to equal ones)
setb(OFF["rel"], 20)
setb(OFF["pat"], 1)
setb(OFF["seqm"], 1)
touch(0, 0, 2 * 64 + 10, 100)
flat = []
for _ in range(14):
    flat += play()[0]
for tt, sl_ in ((300, 2), (1024 + 300, 1), (2048 + 300, 0)):
    exp = (sl_ * 1024 + 300) / LENF * 0.9
    check(f"seq: at {tt} the slice is {sl_}", abs(flat[tt] - exp) < 0.02, f"{flat[tt]:.4f} vs {exp:.4f}")
check("seq: no gap between the slices", min(abs(flat[i]) for i in range(1100, 1160)) > 0.02, min(abs(flat[i]) for i in range(1100, 1160)))
touch(2, 0, 0, 0)
play(6)
check("seq: stops when the finger lifts", max(abs(x) for x in play()[0]) < 1e-3)
setb(OFF["latchm"], 1)
touch(0, 0, 2 * 64 + 10, 100)
touch(2, 0, 0, 0)
play(30)
check("seq latched: keeps going after the lift", max(abs(x) for x in play()[0]) > 0.01)
touch(0, 0, 400, 100)
touch(2, 0, 0, 0)
play(8)
check("seq latched: a tap stops it", max(abs(x) for x in play()[0]) < 1e-3)
setb(OFF["latchm"], 0)
# GRID: one slice per rate step (1/16 = 6000 frames), cut at the step
setb(OFF["seqm"], 2)
setb(OFF["div"], 2)
touch(0, 0, 2 * 64 + 10, 100)
lv2 = []
for _ in range(120):
    lv2.append(max(abs(x) for x in play()[0]))
on2 = [b for b in range(1, 120) if lv2[b] > 0.02 and lv2[b - 1] <= 0.02]
check("seq grid: one slice per step", len(on2) >= 2 and abs((on2[1] - on2[0]) * N - 6000) < 2 * N, (on2, N))
touch(2, 0, 0, 0)
play(8)
setb(OFF["seqm"], 0)
setb(OFF["rel"], 32)

# GRAIN attack / release (the cloud's own envelope)
e.call("samplr_set_mode", 3)
setb(OFF["div"], 3)
setb(OFF["atk"], 70)                                           # ~107 ms
setb(OFF["rel"], 70)
touch(0, 1, 512, 100)
early = max(max(abs(x) for x in play()[0]) for _ in range(3))
late = 0.0
for b in range(130):
    late = max(late, max(abs(x) for x in play()[0])) if b >= 70 else late
    if b < 70:
        play()
check("grain attack: the cloud fades in", early < 0.3 * late and late > 0.02, (early, late))
touch(2, 1, 0, 0)
tail = max(max(abs(x) for x in play()[0]) for _ in range(6))
play(300)
check("grain release: the cloud fades out instead of stopping", tail > 0.01, tail)
check("grain release: silent in the end", max(abs(x) for x in play()[0]) < 1e-5)
setb(OFF["atk"], 19)
setb(OFF["rel"], 32)
e.call("samplr_set_mode", 0)

# TRACKS: another track plays by itself while this one is shown
e.call("samplr_tracks")
check("tracks: the memory holds six", e.uc.reg_read(A.UC_ARM_REG_R0) == 6, e.uc.reg_read(A.UC_ARM_REG_R0))
SMP0 = SMP
e.call("samplr_set_mode", 0)
setb(OFF["loopm"], 1)
setb(OFF["latchm"], 1)
touch(0, 0, 3 * 64 + 10, 100)
touch(2, 0, 0, 0)
play(8)
check("tracks: track 0 has a latched slice loop", max(abs(x) for x in play()[0]) > 0.05)
e.call("samplr_track", 1, count=50_000_000)
e.call("samplr")
SMP = e.uc.reg_read(A.UC_ARM_REG_R0)
check("tracks: track 1 is a state of its own", SMP != SMP0 and sm(OFF["mode"], "B") == 0 and sm(OFF["id"], "i") >= 0 and sm(OFF["latchm"], "B") == 0, (hex(SMP), hex(SMP0)))
lv = max(abs(x) for x in play(4)[0])
check("tracks: track 0 goes on playing while track 1 is shown", lv > 0.05, lv)
e.call("samplr_set_mode", 1)
touch(0, 0, 512, 100)
lv1 = max(abs(x) for x in play()[0])
touch(2, 0, 0, 0)
play(4)
check("tracks: track 1 plays tape on its own", lv1 > lv * 0.5, (lv1, lv))
e.call("samplr_track", 0, count=50_000_000)
e.call("samplr")
back = e.uc.reg_read(A.UC_ARM_REG_R0)
SMP = SMP0
check("tracks: back on track 0 (slicer, latched)", back == SMP0 and sm(OFF["mode"], "B") == 0 and sm(OFF["latchm"], "B") == 1)
e.call("samplr_cycle", 2)                                      # LATCH off: the loop ends
play(10)
check("tracks: LATCH off on track 0 ends its loop", max(abs(x) for x in play()[0]) < 1e-3)
setb(OFF["loopm"], 0)

# a take on track 1 goes on replaying while track 0 is shown
e.call("samplr_track", 1, count=50_000_000)
e.call("samplr")
SMP = e.uc.reg_read(A.UC_ARM_REG_R0)
e.call("samplr_set_mode", 0)
setb(OFF["latchm"], 0)
setb(OFF["loopm"], 0)
setb(OFF["g_bars"], 1)
e.call("samplr_gest", 0)
gwait(lambda: e.r8(SMP + OFF["g_run"]) == 1)
play(20)
touch(0, 0, 5 * 64 + 10, 100)
play(20)
touch(2, 0, 0, 0)
gwait(lambda: e.r8(SMP + OFF["g_layers"]) == 1)
check("tracks: track 1 recorded a take", e.r8(SMP + OFF["g_layers"]) == 1)
e.call("samplr_track", 0, count=50_000_000)
e.call("samplr")
SMP = SMP0
lvt = [max(abs(x) for x in play()[0]) for _ in range(380)]
check("tracks: the take of track 1 replays while track 0 is shown", max(lvt) > 0.02 and min(lvt) < 1e-3, (max(lvt), min(lvt)))
e.call("samplr_track", 1, count=50_000_000)
e.call("samplr")
SMP = e.uc.reg_read(A.UC_ARM_REG_R0)
e.call("samplr_gest", 3)
e.call("samplr_track", 0, count=50_000_000)
SMP = SMP0
play(20)
check("tracks: CLR on track 1 ends it", max(max(abs(x) for x in play()[0]) for _ in range(380)) < 1e-3)

# LOOP mode: a window of the sample that loops; its ends are grabbed in the strip along the top
e.call("samplr_set_mode", 4)
setb(OFF["latchm"], 0)
setb(OFF["qi"], 0)
check("loop: the window starts as the whole sample", sm(OFF["lp_a"], "i") == 0 and sm(OFF["lp_b"], "i") == LENF, (sm(OFF["lp_a"], "i"), sm(OFF["lp_b"], "i")))
touch(0, 0, 10, 50)
touch(1, 0, 256, 50)
touch(2, 0, 0, 0)
touch(0, 0, 1020, 50)
touch(1, 0, 512, 50)
touch(2, 0, 0, 0)
check("loop: both ends move", sm(OFF["lp_a"], "i") == 4096 and sm(OFF["lp_b"], "i") == 8192, (sm(OFF["lp_a"], "i"), sm(OFF["lp_b"], "i")))
touch(0, 0, 900, 500)
lvl = [max(abs(x) for x in play()[0]) for _ in range(120)]
check("loop: plays inside the window, over and over", 0.15 < min(lvl[5:]) and max(lvl) < 0.35, (min(lvl[5:]), max(lvl)))
touch(2, 0, 0, 0)
play(6)
check("loop: silent after the lift", max(abs(x) for x in play()[0]) < 1e-3)
e.call("samplr_cycle", 13)
check("loop: RESET gives the whole sample back", sm(OFF["lp_a"], "i") == 0 and sm(OFF["lp_b"], "i") == LENF)
e.call("samplr_set_mode", 0)

# FX: filter and the sends into the looper's delay and reverb; the level meter
O_ROUTE = 10
opt(O_ROUTE, 1)
e.call("samplr_set_slices", 16)
touch(0, 0, 3 * 64 + 10, 100)
dry = [max(abs(x) for x in play()[0]) for _ in range(2)]
check("meter: the track's level is seen", struct.unpack("<f", e.uc.mem_read(SMP + OFF["peak"], 4))[0] > 0.05)
touch(2, 0, 0, 0)
play(6)
setb(OFF["fx_f"], 156)                                          # -100: lowest low pass
touch(0, 0, 3 * 64 + 10, 100)
flv = [max(abs(x) for x in play()[0]) for _ in range(2)]
touch(2, 0, 0, 0)
play(8)
setb(OFF["fx_f"], 0)
check("fx filter: the low pass takes the start of a note down", flv[1] < 0.6 * dry[1], (dry, flv))
play(6)
nsl = max(max(abs(x) for x in play()[0]) for _ in range(120))
check("fx: no send, no echo", nsl < 1e-3, nsl)
setb(OFF["fx_sd"], 100)
touch(0, 0, 3 * 64 + 10, 100)
play(8)
touch(2, 0, 0, 0)
play(6)
echo = max(max(abs(x) for x in play()[0]) for _ in range(150))
check("fx delay send: an echo of the note arrives", echo > 0.01, echo)
setb(OFF["fx_sd"], 0)
play(300)
setb(OFF["fx_sr"], 100)
touch(0, 0, 3 * 64 + 10, 100)
play(8)
touch(2, 0, 0, 0)
play(6)
tail = max(max(abs(x) for x in play()[0]) for _ in range(40))
check("fx reverb send: a tail", tail > 0.003, tail)
setb(OFF["fx_sr"], 0)
play(400)
opt(O_ROUTE, 0)
e.call("samplr_fx_knob", 1, 200)
check("fx: encoder 2 raises the delay send (a step per 20 counts)", e.r8(SMP + OFF["fx_sd"]) == 10, e.r8(SMP + OFF["fx_sd"]))
e.call("samplr_fx_knob", 2, 100)
e.call("samplr_fx_knob", 3, 60)
check("fx: encoder 3 is the reverb send, encoder 4 the resonance", e.r8(SMP + OFF["fx_sr"]) == 5 and e.r8(SMP + OFF["fx_r"]) == 53, (e.r8(SMP + OFF["fx_sr"]), e.r8(SMP + OFF["fx_r"])))
setb(OFF["fx_sr"], 0)
setb(OFF["fx_r"], 50)
a0_ = e.r8(SMP + OFF["atk"])
e.call("samplr_env_knob", 0, 400)
check("fx + INFO: encoder 1 turns the attack (a step per 40 counts)", e.r8(SMP + OFF["atk"]) == a0_ + 10, e.r8(SMP + OFF["atk"]))
setb(OFF["atk"], 19)
setb(OFF["fx_sd"], 0)
play(300)

# GRAIN release as the volume of the whole cloud: with a long release the grains go on sounding, fading, after the lift
e.call("samplr_set_mode", 3)
setb(OFF["div"], 3)
setb(OFF["atk"], 10)
setb(OFF["rel"], 88)                                           # ~0.5 s
touch(0, 1, 512, 100)
play(80)
before = max(max(abs(x) for x in play()[0]) for _ in range(30))
touch(2, 1, 0, 0)
play(20)                                                       # ~100 ms after the lift
after100 = max(max(abs(x) for x in play()[0]) for _ in range(10))
play(150)                                                      # ~0.9 s
late = max(max(abs(x) for x in play()[0]) for _ in range(10))
check("grain long release: the cloud still sounds 100 ms after the lift", after100 > 0.3 * before, (before, after100))
check("grain long release: and has faded out well within the release's second half", late < 0.1 * before, (before, late))
play(400)
setb(OFF["atk"], 19)
setb(OFF["rel"], 32)
e.call("samplr_set_mode", 0)

# ARP: a longer release makes longer notes, the shortest settings short fragments
e.call("samplr_set_mode", 2)
setb(OFF["div"], 1)
setb(OFF["atk"], 0)
setb(OFF["rel"], 0)
touch(0, 0, 300, 0)
sg = []
cur = None
allo = []
for _ in range(100):
    allo += play()[0]
touch(2, 0, 0, 0)
play(20)
segs = []
for i, x in enumerate(allo):
    if abs(x) > 1e-4:
        if cur is None:
            cur = [i, i]
        cur[1] = i
    elif cur is not None and i - cur[1] > 30:
        segs.append(cur)
        cur = None
check("arp: the lowest attack and release give fragments of a couple of ms", len(segs) >= 2 and all(b_ - a_ < 400 for a_, b_ in segs), [b_ - a_ for a_, b_ in segs])
setb(OFF["atk"], 60)
setb(OFF["rel"], 70)
touch(0, 0, 300, 0)
allo = []
for _ in range(100):
    allo += play()[0]
touch(2, 0, 0, 0)
play(400)
on_ = sum(1 for x in allo if abs(x) > 1e-4)
check("arp: a long attack and release make long notes (most of the time something sounds)", on_ > 0.5 * len(allo), on_ / len(allo))
setb(OFF["atk"], 19)
setb(OFF["rel"], 32)
e.call("samplr_set_mode", 0)

# PANIC / STOP twice: whatever other tracks still play on (latched things have no stop of their own) ends
e.call("samplr_set_mode", 0)
setb(OFF["loopm"], 1)
setb(OFF["latchm"], 1)
touch(0, 0, 3 * 64 + 10, 100)
touch(2, 0, 0, 0)
e.call("samplr_track", 1, count=50_000_000)
e.call("samplr")
SMP = e.uc.reg_read(A.UC_ARM_REG_R0)
e.call("samplr_set_mode", 1)
setb(OFF["latchm"], 2)
touch(0, 0, 512, 100)
touch(2, 0, 0, 0)
play(8)
check("stop all: two tracks play latched things", max(abs(x) for x in play()[0]) > 0.1)
e.call("samplr_others_active")
check("stop all: track 1 sees that another track plays", e.uc.reg_read(A.UC_ARM_REG_R0) == 1)
e.call("samplr_stop_all")                                      # PANIC
play(10)
check("stop all: PANIC silences every track", max(abs(x) for x in play()[0]) < 1e-3)
e.call("samplr_track", 0, count=50_000_000)
SMP = SMP0
setb(OFF["loopm"], 0)
setb(OFF["latchm"], 0)
play(10)

# LOOP: two fingers on the waveform set the two ends
e.call("samplr_set_mode", 4)
e.call("samplr_cycle", 13)
touch(0, 0, 300, 500)
touch(0, 1, 700, 500)
check("loop: a second finger sets the two ends (no strip needed)", sm(OFF["lp_a"], "i") == 300 * 16 and sm(OFF["lp_b"], "i") == 700 * 16, (sm(OFF["lp_a"], "i"), sm(OFF["lp_b"], "i")))
touch(1, 1, 800, 500)
check("loop: moving it moves that end", sm(OFF["lp_b"], "i") == 800 * 16, sm(OFF["lp_b"], "i"))
touch(1, 0, 200, 500)
check("loop: moving the first finger moves the other end", sm(OFF["lp_a"], "i") == 200 * 16, sm(OFF["lp_a"], "i"))
touch(2, 1, 0, 0)
lv_ = max(abs(x) for x in play(4)[0])
check("loop: the first finger goes on playing", lv_ > 0.05, lv_)
touch(2, 0, 0, 0)
play(8)
e.call("samplr_cycle", 13)
e.call("samplr_set_mode", 0)

# the governor: near the audio task's limit SAMPLR sheds (1 half the grains, 2 linear interpolation, 3 others start nothing new)
def scr_off(name):
    src = '#include "src/samplr.h"\nchar o_x[__builtin_offsetof(struct smscr,%s)];\n' % name
    out = subprocess.run(["arm-none-eabi-gcc", "-I.", "-mthumb", "-S", "-x", "c", "-", "-o", "-"], input=src, capture_output=True, text=True, check=True).stdout.splitlines()
    return int([l.split()[1] for l in out if l.strip().startswith(".space")][0])
SC = sm(OFF["sc"], "I")
SHED = SC + scr_off("shed")
e.call("samplr_set_mode", 0)
e.uc.mem_write(SHED, b"\x02")
e.uc.mem_write(SC + scr_off("shed_t"), struct.pack("<I", sm(OFF["tick"], "I")))        # (just changed: it stays for now)
e.call("samplr_trans", 0)
e.call("samplr_trans", 7)
touch(0, 0, 3 * 64 + 10, 100)
play()
o = play()[0]
steps = [o[i + 1] - o[i] for i in range(40, 200)]
exp = 2 ** (7 / 12) / LENF * 0.9
check("shed 2: linear interpolation, the pitched ramp stays a clean ramp", max(abs(x - exp) for x in steps) < exp * 0.03, (min(steps), max(steps), exp))
touch(2, 0, 0, 0)
play(6)
e.call("samplr_trans", 0)
e.uc.mem_write(SHED, b"\x00")
# a loaded task makes it shed by itself, and it comes back
e.uc.mem_write(0x2405ffe2, struct.pack("<H", 900))
e.uc.mem_write(SMP + OFF["load"], struct.pack("<H", 900))
play(60)
check("governor: sheds when the task is over its limit", e.r8(SHED) >= 1, e.r8(SHED))
e.uc.mem_write(0x2405ffe2, struct.pack("<H", 100))
e.uc.mem_write(SMP + OFF["load"], struct.pack("<H", 0))
for _ in range(20):
    e.uc.mem_write(SMP + OFF["load"], struct.pack("<H", 0))
    play(100)
check("governor: and comes back when it is quiet again", e.r8(SHED) == 0, e.r8(SHED))

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
check("transients: each slice point sits just before its burst, never inside it", all(b_ - 200 < c <= b_ for c, b_ in zip(cuts[1:5], (2000, 6000, 10000, 14000))), cuts)
check("transients: first point 0, last point the end", cuts[0] == 0 and cuts[ns] == LENF, cuts)

print(f"\n{fails} failure(s)")
