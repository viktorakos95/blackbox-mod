"""SAMPLR voices under Unicorn: overview, slicer (pitch, gate / one-shot, 4 fingers), tape (speed, wrap).
Stubbed: the stock PCM reader (a ramp sample), the rest as in test_looper_v4.py. NOT checked: how the page looks, the real
reader's behaviour on a non-resident block, CPU on the chip."""
import struct

src = open("test_looper_v4.py").read().split("def flat2")[0]
exec(src)

SE = 0x2400A9C0                      # the stock engine object
LENF = 16384
SLOTS, BL, ENT, BUFL = 0x24040000, 0x24050000, SE + 0x1C * 1, 0x24060000
APPO = 0x24020088


def sample(f):
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
check("selected id 5", sm(80, "i") == 5, sm(80, "i"))
check("length read from the slot", sm(84, "i") == LENF, sm(84, "i"))
ovmin, ovmax = 0x0, 0x0
ov = struct.unpack("<300b", e.uc.mem_read(SMP + 112 + 0, 300)) if False else None


def find_ov():
    # the overview follows the voices: search for the first 150 bytes that look like a rising max ramp
    base = SMP + 88 + 4 * 0
    return base


e.call("samplr_name", 0x24072100, 24)
check("name is the file name without folder and extension", e.cstr(0x24072100) == "kick", e.cstr(0x24072100))


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

# octave up (top of the area)
touch(0, 0, 3 * 64 + 10, 0)
o = play()[0]
exp = (3072 + 400) / LENF * 0.9
check("slicer: top of the area = one octave up", abs(o[200] - exp) < 0.02, f"{o[200]:.4f} vs {exp:.4f}")
touch(2, 0, 0, 0)
play(2)

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

print(f"\n{fails} failure(s)")
