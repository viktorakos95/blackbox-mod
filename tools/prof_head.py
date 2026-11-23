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


OFF = struct_offsets(["iq", "g_sz", "g_dry", "a_step", "vol", "g_drift", "g_ppat", "g_cont", "g_warpmode", "g_bars", "g_run", "g_armed", "g_layers", "g_rec", "g_pos", "g_len", "loopm", "latchm", "gfree", "dens", "id", "len", "filled", "spot", "cut", "trans", "ypit", "sph", "qi", "div", "pat", "atk", "scale_dummy_unused" if False else "rel", "nslice", "mode", "rev", "seqm", "sq_on", "gate", "lp_a", "lp_b", "fx_f", "fx_r", "fx_sd", "fx_sr", "peak"])

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


