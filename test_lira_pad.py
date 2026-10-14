"""LIRA-8 on a pad (src/lira_pad.c + the filter.c hooks) under Unicorn, on the patched image.

Real firmware runs for: the settings store (lookup / add), the list add / clear, the audio-buffer accessors.
Stubbed: the registrars, the SDRAM allocator (returns a mapped area), the page builder and pad-record lookup (fake
cell), the voice render after LIRA (Level / ADSR / filter / mix: the test reads what LIRA handed it).
Time comes from the DWT cycle counter, written by the test between blocks.
NOT checked: the knob screen itself, the Keys screen, sound against the Pd original, CPU on the device.
"""
import math
import struct
import sys

from capstone import Cs, CS_ARCH_ARM, CS_MODE_MCLASS, CS_MODE_THUMB
from unicorn import UC_HOOK_BLOCK
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_S0, UC_ARM_REG_SP

from emu import Emu

IMG = "out/solo+slice+duck+chord+cond+filter+cpu+od+comp+seqfix+fx2+munchi+lira+cave/BLACKBOX.bin"
e = Emu(IMG, "out/cave.elf")
e.uc.mem_map(0xC0000000, 0x200000)               # SDRAM heap
e.uc.mem_map(0x38800000, 0x1000)                 # backup SRAM
e.uc.mem_map(0x58024000, 0x1000)                 # RCC + PWR
e.uc.mem_map(0xE0000000, 0x10000)                # DWT, DEMCR

# instruction counter (installed before anything runs: Unicorn does not hook blocks it has already translated)
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB + CS_MODE_MCLASS)
cache, ins, counting = {}, [0], [False]


def blk(uc, a, s, u):
    if counting[0]:
        if a not in cache:
            cache[a] = len(list(md.disasm(bytes(uc.mem_read(a, s)), a)))
        ins[0] += cache[a]


e.uc.hook_add(UC_HOOK_BLOCK, blk)

fails = 0


def check(name, ok, info=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + ("  " + info if info else ""))
    fails += not ok


P0, NCTL = 0x1DB, 25
INTERP_LIRA = 14

# ---- 1. registration at boot: Fidelity gains LIRA, 25 controls registered, memory taken once ----
regs, allocs = [], []
e.stub(0x0808C0D8, lambda: regs.append(("list", e.arg(1), e.arg(3), e.r32(e.uc.reg_read(UC_ARM_REG_SP)))))
e.stub(0x0808C12C, lambda: regs.append(("num", e.arg(1), e.arg(2), e.r32(e.uc.reg_read(UC_ARM_REG_SP) + 4))))
e.stub(0x0804436C, lambda: allocs.append(e.arg(0)), value=0xC0000000)
e.w32(0x24000088, 0xC4000000)                                   # the allocator's top and floor: 64 MB free
e.w32(0x2400008C, 0xC0000000)
e.uc.mem_write(0x38800BF0, b"\xa5" * 8)          # whatever the last boot left
e.call("interp_register", 0x24060000, 0xF1, 0, 0, 2, 0)
fid = [r for r in regs if r[1] == 0xF1]
check("Fidelity list: 15 entries, the last one LIRA",
      len(fid) == 1 and fid[0][3] == 15 and e.cstr(e.r32(fid[0][2] + 4 * INTERP_LIRA) & 0xFFFFFFFF) == "DRONE-8",
      str(fid))
ids = sorted(r[1] for r in regs if r[1] != 0xF1)
check("25 LIRA controls at 0x1db..0x1f3", ids == list(range(P0, P0 + NCTL)), str([hex(i) for i in ids]))
check("percent controls are 0..1000", all(r[2] == 8 and r[3] == 1000 for r in regs if r[0] == "num"))
check("one SDRAM allocation, 0.5..2 MB", len(allocs) == 1 and 1 << 19 < allocs[0] < 2 << 20, str(allocs))
check("pool pointer in backup SRAM, rewritten this boot",
      e.r32(0x38800BF0) & 0xFFFFFFFF == 0xC0000000 and e.r32(0x38800BF4) & 0xFFFFFFFF == 0x4C495241 ^ 0xC0000000)

# ---- 2. the pad stores: LIRA's values live in the pool (a Sample pad's store is full); see test_lira_ui.py for pages ----
APP_PADS = 0x24020088 + 0x1840
POOL_SIDE = 0xC0000000 + 4                       # side[pad position][control] int16, first in the pool
got = {i: e.r16(POOL_SIDE + 2 * i) for i in range(NCTL)}     # the first position: LIRA-8's defaults
check("pool: every pad position starts at LIRA-8's defaults (Tune 2 = 78/127, Touch = Fast, Ext In = 0)",
      got[1] == round(78 * 1000 / 127) and got[24] == 1 and got[21] == 0, str(got))
DEFAULTS = {P0 + i: got[i] for i in range(NCTL)}
SLOT0 = 4 + 2 * 20 * NCTL                        # the pool's first instance (its pad pointer)


def make_store(addr, arr, entries, cap=64, vtable=0):
    data = b"".join(struct.pack("<HHI", k, 0, v & 0xFFFFFFFF) for k, v in entries)
    e.uc.mem_write(arr, data + b"\0" * (cap * 8 - len(data)))
    e.uc.mem_write(addr, struct.pack("<IIHHH", vtable, arr, 0, len(entries), cap))   # count @+0xa, capacity @+0xc


# ---- 4. playing: the voice's audio is replaced before the voice render (Level, ADSR, filter) ----
PADS = [0x24063000, 0x24063200, 0x24063400]
OBJ, OBJ2, BUF, LBUF, RBUF, SRC = 0x24064000, 0x24064400, 0x24064800, 0x24065000, 0x24066000, 0x24064900
N = 256
rendered = []
e.stub(0x08055BB8, lambda: rendered.append((e.arg(0), e.arg(1), e.arg(2), e.arg(3))))   # the stock voice render


def app_pad(i, fidelity=INTERP_LIRA, extra=()):
    row, col = 1, i
    store, arr = APP_PADS + row * 0xF0 + col * 0x30, 0x24068000 + i * 0x400
    make_store(store, arr, [(0x8B, 0), (0xF1, fidelity)])
    for k, v in {**DEFAULTS, **dict(extra)}.items():
        e.call("lira_set_c", store, k, v)
    e.w32(PADS[i] + 0x18, row << 8 | col)


now = [1000]


def voice(pad, obj=OBJ, rate=1.0, advance=True):
    if advance:
        now[0] += 2_560_000                       # one 256-frame block at 480 MHz
    e.w32(0xE0001004, now[0])
    e.w32(0x2405FFF4, pad)                        # the voice reader's park: this pad, its rate
    e.uc.mem_write(0x2405FFFC, struct.pack("<f", rate))
    e.uc.mem_write(LBUF, struct.pack(f"<{N}f", *([0.25] * N)))
    e.uc.mem_write(RBUF, struct.pack(f"<{N}f", *([0.25] * N)))
    e.uc.mem_write(BUF, struct.pack("<IIIIBB", N, N, LBUF, RBUF, 0, 1))
    e.uc.mem_write(SRC, struct.pack("<IIIIBB", N, N, 0, 0, 0, 1))
    rendered.clear()
    e.call("lira_render", pad, obj, BUF, SRC, count=5_000_000)
    assert rendered == [(pad, obj, BUF, SRC)], rendered       # the stock render always follows, same arguments
    l = struct.unpack(f"<{N}f", e.uc.mem_read(LBUF, 4 * N))
    r = struct.unpack(f"<{N}f", e.uc.mem_read(RBUF, 4 * N))
    return l, r


def rms(x):
    return math.sqrt(sum(v * v for v in x) / len(x))


for i in range(3):
    app_pad(i)
outs = []
counting[0] = True
voice(PADS[0])                                    # the first block claims an instance
counting[0] = False
first = ins[0]
for b in range(59):
    l, r = voice(PADS[0])
    outs += l
check("LIRA pad: the sample is replaced (no 0.25 left), finite, bounded",
      all(math.isfinite(v) and abs(v) <= 1.0 for v in outs) and 0.25 not in outs[:N])
check("LIRA pad: audible once the touch envelopes open", rms(outs[-20 * N:]) > 0.01, "rms %.4f" % rms(outs[-20 * N:]))
check("LIRA pad: left = right", l == r)
l2, _ = voice(PADS[0], obj=OBJ2, advance=False)
check("a second voice of the same pad in the same block: silent (it only holds sensors)", max(map(abs, l2)) == 0.0)

# a pad that is not LIRA keeps its sample
app_pad(1, fidelity=0)
l, _ = voice(PADS[1])
check("Fidelity Normal: the sample passes untouched", all(v == 0.25 for v in l))

# instances: two LIRA pads play at once, a third waits while both play
app_pad(1)
for b in range(4):
    voice(PADS[0])
    lb, _ = voice(PADS[1], advance=False)
    lc, _ = voice(PADS[2], advance=False)
check("second LIRA pad gets its own instance", 0.25 not in lb and max(map(abs, lb)) > 0.0, "peak %.4f" % max(map(abs, lb)))
check("third LIRA pad while two play: silent", max(map(abs, lc)) == 0.0)
for b in range(30):                               # pads 0 and 1 stop; pad 2 plays alone
    lc, _ = voice(PADS[2])
check("third pad takes an idle instance", rms(lc) > 0.0, "rms %.4f" % rms(lc))

# sensors from pitch: one sensor (3 semitones up) is quieter than the root's eight
def level_with(rate, blocks=80):
    for i in range(3):
        app_pad(i, extra={P0 + 19: 0})            # delay mix 0, for a clean level
    now[0] += 3_000_000_000                       # a long silence: fresh instances
    e.uc.mem_write(0xC0000000 + SLOT0, b"\0" * 4)
    out = []
    for b in range(blocks):
        l, _ = voice(PADS[0], rate=rate)
        out += l
    return rms(out[-30 * N:])


root = level_with(1.0)
one = level_with(2 ** (3 / 12))
check("root note holds all 8 sensors, +3 semitones holds one (quieter)", root > one > 0.0,
      "root %.4f one %.4f" % (root, one))

# Ext In: the block the audio task converted (engine + 0x8fb0 / 0x8fb4) is mixed in; silent at 0
ENG, XL, XR = 0x24070000, 0x24071000, 0x24072000
tone = [0.4 * math.sin(2 * math.pi * 200 * i / 48000) for i in range(N)]
e.uc.mem_write(XL, struct.pack(f"<{N}f", *tone))
e.uc.mem_write(XR, struct.pack(f"<{N}f", *tone))
e.w32(ENG + 0x8FB0, XL)
e.w32(ENG + 0x8FB4, XR)
e.w32(0x38800BF8, ENG)
e.w32(0x38800BFC, N)


def ext_level(g):
    for i in range(3):
        app_pad(i, extra={P0 + 19: 0, P0 + 21: g})
    now[0] += 3_000_000_000
    e.uc.mem_write(0xC0000000 + SLOT0, b"\0" * 4)
    out = []
    for b in range(80):
        l, _ = voice(PADS[0], rate=2 ** (3 / 12))
        out += l
    return rms(out[-30 * N:])


quiet, loud = ext_level(0), ext_level(1000)
check("Ext In: the input block is mixed in (louder with it up, same pad otherwise)", loud > 1.2 * quiet and loud < 2.0,
      "Ext 0: %.4f, Ext max: %.4f" % (quiet, loud))

# Touch = Follow: the envelopes follow the input's level (silent without input, sounding with it); FM Source Ext stays finite
def follow_level(amp, extra=None):
    e.uc.mem_write(XL, struct.pack(f"<{N}f", *[amp * v / 0.4 for v in tone]))
    e.uc.mem_write(XR, struct.pack(f"<{N}f", *[amp * v / 0.4 for v in tone]))
    for i in range(3):
        app_pad(i, extra={P0 + 19: 0, P0 + 21: 0, P0 + 24: 2, **(extra or {})})
    now[0] += 3_000_000_000
    e.uc.mem_write(0xC0000000 + SLOT0, b"\0" * 4)
    out = []
    for b in range(80):
        l, _ = voice(PADS[0], rate=2 ** (3 / 12))
        out += l
    return rms(out[-30 * N:]), all(math.isfinite(v) and abs(v) <= 1.0 for v in out)


silent, _ = follow_level(0.0)
driven, fine = follow_level(0.4)
check("Touch Follow: the voices sound with input and not without", driven > 5 * silent and driven > 0.003 and fine,
      "no input %.5f, input %.5f" % (silent, driven))
_, fine = follow_level(0.4, {P0 + 12: 3, P0 + 11: 900})
check("FM Source Ext at high depth: finite and bounded", fine)

# CPU: instructions for one block of one playing instance (a Unicorn count, not cycles)
ins[0] = 0
counting[0] = True
voice(PADS[0])
counting[0] = False
print("INFO %d instructions for one 256-frame block (%.0f per frame)" % (ins[0], ins[0] / N))
check("claiming an instance costs no more than 1.5 playing blocks (delay lines cleared a slice per block)",
      first < 1.5 * ins[0], "first block %d, playing block %d" % (first, ins[0]))

sys.exit(1 if fails else 0)
