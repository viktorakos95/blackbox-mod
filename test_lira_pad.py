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
e.uc.mem_write(0x38800F00, b"\xa5" * 8)          # whatever the last boot left
e.call("interp_register", 0x24060000, 0xF1, 0, 0, 2, 0)
fid = [r for r in regs if r[1] == 0xF1]
check("Fidelity list: 15 entries, the last one LIRA",
      len(fid) == 1 and fid[0][3] == 15 and e.cstr(e.r32(fid[0][2] + 4 * INTERP_LIRA) & 0xFFFFFFFF) == "LIRA",
      str(fid))
ids = sorted(r[1] for r in regs if r[1] != 0xF1)
check("25 LIRA controls at 0x1db..0x1f3", ids == list(range(P0, P0 + NCTL)), str([hex(i) for i in ids]))
check("percent controls are 0..1000", all(r[2] == 8 and r[3] == 1000 for r in regs if r[0] == "num"))
check("one SDRAM allocation, 1..2 MB", len(allocs) == 1 and 1 << 20 < allocs[0] < 2 << 20, str(allocs))
check("pool pointer in backup SRAM, rewritten this boot",
      e.r32(0x38800F00) & 0xFFFFFFFF == 0xC0000000 and e.r32(0x38800F04) & 0xFFFFFFFF == 0x4C495241 ^ 0xC0000000)

# ---- 2. pad defaults: + the 25 controls, within the store's 64 ----
STORE, ARR = 0x24061000, 0x24061100


def make_store(addr, arr, entries, cap=64, vtable=0):
    data = b"".join(struct.pack("<HHI", k, 0, v & 0xFFFFFFFF) for k, v in entries)
    e.uc.mem_write(arr, data + b"\0" * (cap * 8 - len(data)))
    e.uc.mem_write(addr, struct.pack("<IIHHH", vtable, arr, 0, len(entries), cap))   # count @+0xa, capacity @+0xc


make_store(STORE, ARR, [(0x100 + i, 0) for i in range(36)])
e.call("lira_defaults", STORE, 0xF1, 0)
n = e.r16(STORE + 10)
got = {e.r16(ARR + 8 * i): e.r32(ARR + 8 * i + 4) for i in range(n)}
check("defaults: Fidelity + 25 controls, 62 of 64", n == 62 and 0xF1 in got and all(P0 + i in got for i in range(NCTL)),
      "count %d" % n)
check("defaults: Tune 2 = 78/127, Touch = Fast, Mod Src = LFO",
      got.get(P0 + 1) == round(78 * 1000 / 127) and got.get(P0 + 24) == 1 and got.get(P0 + 21) == 2)
DEFAULTS = {k: v for k, v in got.items() if P0 <= k < P0 + NCTL}

# ---- 3. pages: a LIRA Sample pad's pages 2 and 3 list LIRA's controls ----
APP, CELL, LIST, REC, RARR, VT = 0x24020088, 0x24062000, 0x24062100, 0x24062600, 0x24062700, 0x24062F00
e.w32(VT, 0x0807FF01)
e.stub(0x0807FF00, value=0x4C)                    # the record's vtable slot 0: cell type = pad
e.stub(0x08098D0C, value=REC)                    # pad record lookup


def stock_pages():                               # the stock builder: one entry, whatever the page
    e.uc.mem_write(LIST, struct.pack("<I", 1) + struct.pack("<HHHHI", 0x777, 0, 0, 0, 5))   # entry: id @+4, value @+0xc


e.stub(0x08098D40, stock_pages, value=1)


def pad_record(fidelity, mode=0, extra=()):
    make_store(REC, RARR, [(0x8B, mode), (0xF1, fidelity), (0x8E, 1)] + list(DEFAULTS.items()) + list(extra), vtable=VT)


def page(p, cell_id=0x0012):
    e.uc.mem_write(CELL + 4, struct.pack("<H", cell_id))
    e.call("lira_pages", APP, CELL, LIST, p)
    r = e.uc.reg_read(UC_ARM_REG_R0)
    cnt = e.r32(LIST)
    return r, [e.r16(LIST + 12 * i + 4) for i in range(cnt)]


pad_record(INTERP_LIRA)
r, p1 = page(1)
check("page 2: Loop Mode, Sharp, Tune 1..8, Pitch, Hold",
      r == 1 and p1 == [0x8E, P0 + 9] + list(range(P0, P0 + 9)) + [P0 + 10], str([hex(x) for x in p1]))
r, p2 = page(2)
check("page 3: Touch, FM Depth/Source, LFO A/B, Drive, Dist Mix, then the delay, Vibrato last",
      p2 == [P0 + k for k in (24, 11, 12, 14, 15, 22, 23, 16, 17, 18, 19, 20, 21, 13)], str([hex(x) for x in p2]))
check("every control is on a page exactly once", sorted(p1[1:] + p2) == list(range(P0, P0 + NCTL)))
r, p0 = page(0)
check("pages 1, 4, 5 unchanged", p0 == [0x777] and page(4)[1] == [0x777])
pad_record(0)
check("Fidelity Normal: stock pages", page(1)[1] == [0x777])
pad_record(INTERP_LIRA, mode=2)
check("Slicer pad: stock pages", page(1)[1] == [0x777])
pad_record(INTERP_LIRA)
check("FX cell (not a pad): stock pages", page(1, cell_id=0x0312)[1] == [0x777])

# ---- 4. playing: the voice's audio is replaced before the voice render (Level, ADSR, filter) ----
APP_PADS = 0x24020088 + 0x1840
PADS = [0x24063000, 0x24063200, 0x24063400]
OBJ, OBJ2, BUF, LBUF, RBUF, SRC = 0x24064000, 0x24064400, 0x24064800, 0x24065000, 0x24066000, 0x24064900
N = 256
rendered = []
e.stub(0x08055BB8, lambda: rendered.append((e.arg(0), e.arg(1), e.arg(2), e.arg(3))))   # the stock voice render


def app_pad(i, fidelity=INTERP_LIRA, extra=(), lira_entries=True):
    row, col = 1, i
    store, arr = APP_PADS + row * 0xF0 + col * 0x30, 0x24068000 + i * 0x400
    lira = {**DEFAULTS, **dict(extra)} if lira_entries else {}
    make_store(store, arr, [(0x8B, 0), (0xF1, fidelity)] + list(lira.items()))
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

# a pad from before 3.1.o: Fidelity LIRA but no LIRA entries in its store -> LIRA's defaults, not all-zero (clicks)
def drone(**kw):
    app_pad(0, **kw)
    now[0] += 3_000_000_000
    e.uc.mem_write(0x38800F00 - 0x38800F00 + 0xC0000000 + 4, b"\0" * 4)   # free the instances: start afresh
    out = []
    for b in range(60):
        l, _ = voice(PADS[0])
        out += l
    tail = out[-20 * N:]
    return rms(tail), sum(1 for a, b in zip(tail, tail[1:]) if (a < 0) != (b < 0))


zero = drone(extra={P0 + i: 0 for i in range(NCTL)})
missing = drone(lira_entries=False)
check("pad without LIRA entries plays LIRA's defaults, not the all-zero settings 3.1.o read (clicks)",
      missing[0] > 0.01 and missing[1] > 4 * max(zero[1], 1),
      "missing: rms %.3f, %d crossings; all zero: rms %.3f, %d crossings" % (missing + zero))
pad_record(INTERP_LIRA)
make_store(REC, RARR, [(0x8B, 0), (0xF1, INTERP_LIRA), (0x8E, 1)], vtable=VT)
page(1)
n = e.r16(REC + 10)
have = {e.r16(RARR + 8 * i) for i in range(n)}
check("opening a LIRA page adds the missing entries (so the knobs work and save)",
      all(P0 + i in have for i in range(NCTL)) and n == 3 + NCTL, "count %d" % n)
page(2)
check("... once", e.r16(REC + 10) == 3 + NCTL)

# sensors from pitch: one sensor (3 semitones up) is quieter than the root's eight
def level_with(rate, blocks=80):
    for i in range(3):
        app_pad(i, extra={P0 + 19: 0})            # delay mix 0, for a clean level
    now[0] += 3_000_000_000                       # a long silence: fresh instances
    e.uc.mem_write(0xC0000000 + 4, b"\0" * 4)
    out = []
    for b in range(blocks):
        l, _ = voice(PADS[0], rate=rate)
        out += l
    return rms(out[-30 * N:])


root = level_with(1.0)
one = level_with(2 ** (3 / 12))
check("root note holds all 8 sensors, +3 semitones holds one (quieter)", root > one > 0.0,
      "root %.4f one %.4f" % (root, one))

# CPU: instructions for one block of one playing instance (a Unicorn count, not cycles)
ins[0] = 0
counting[0] = True
voice(PADS[0])
counting[0] = False
print("INFO %d instructions for one 256-frame block (%.0f per frame)" % (ins[0], ins[0] / N))
check("claiming an instance costs no more than 1.5 playing blocks (delay lines cleared a slice per block)",
      first < 1.5 * ins[0], "first block %d, playing block %d" % (first, ins[0]))

sys.exit(1 if fails else 0)
