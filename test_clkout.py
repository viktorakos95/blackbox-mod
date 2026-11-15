"""The clock pulse on Out 2 (src/cpu.c clkout_block / clkout_mix, options CLKPPQ / CLKLVL = the looper's RSIZE / RRET slots) under Unicorn.

The looper is booted for its state (the options live there); the sequencer's stamp (seqfix.c's clock block, 0x38800908) and the stock clock object (tempo +0x18, running
+0x38) are written by the test; each block is clkout_block then clkout_mix on a zeroed bus. Checked: off by default; pulses fall on the exact frame (positions in
ticks, 960 a beat, rate = bpm * 960 / 2.88e6 ticks a frame), once each, across many blocks, at every PPQ; the level; a pulse that straddles a block; a locate backwards
and a jump; a tempo change; without stamps (the transport's running flag): from its start; stopped: nothing.
NOT checked (hardware): where the stamp sits inside the block against the stock sequencer's notes, the jack's coupling and levels.
"""
import struct
import sys

from emu import Emu

e = Emu("out/solo+slice+duck+chord+cond+filter+cpu+od+comp+seqfix+fx2+munchi+looper+cave/BLACKBOX.bin", "out/cave.elf")
e.uc.mem_map(0x38800000, 0x1000)
e.uc.mem_map(0x58024000, 0x1000)
e.uc.mem_map(0xE0001000, 0x1000)
e.uc.mem_map(0xE000E000, 0x1000)
e.uc.mem_map(0xC0000000, 0x1400000)
e.uc.mem_map(0x30000000, 0x40000)
ENGINE, STATE = 0x30000000, 0x38800C00
ENTRIES, PER, AREAS, FXE = 615, 45, 5, 6
FIRST = ENTRIES - AREAS * PER - FXE
BUFSET, CTXB, BUSL, BUSR = 0x30038100, 0x30039000, 0x30020000, 0x30021000
CLK_LO, CLK_HI = 0x38800908, 0x3880090C
fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


e.uc.mem_write(ENGINE, bytes(0x1C * ENTRIES + 8))
for i in range(ENTRIES):
    if i >= FIRST:
        k = i - FIRST
        e.w32(ENGINE + 0x1C * i + 4, 0xC0000000 + k * 0x10000)
        e.w32(ENGINE + 0x1C * i + 8, 0xC0000000 + k * 0x10000 + 0x8000)
    else:
        e.w32(ENGINE + 0x1C * i + 4, 0x30030000)
        e.w32(ENGINE + 0x1C * i + 8, 0x30030000)
e.stub(0x08070BF4, lambda: None)
e.uc.mem_write(0xC0000000, b"\x11" * 0x100)
e.call("looper_boot", ENGINE, count=200_000_000)
e.w32(BUFSET, CTXB)


def opt(i, v):
    e.uc.mem_write(STATE + 164 + i * 4, struct.pack("<f", v))


def stamp(ticks):
    e.w32(CLK_LO, int(ticks) & 0xFFFFFFFF)
    e.w32(CLK_HI, int(ticks) >> 32)


def transport(run, bpm):
    e.uc.mem_write(CTXB + 0x18, struct.pack("<f", bpm))
    e.uc.mem_write(CTXB + 0x38, bytes([1 if run else 0]))


N = 128


def block(stamp_ticks=None):
    """one block: the pulse's frames (non-zero samples on the left) and whether the right is the same"""
    if stamp_ticks is not None:
        stamp(stamp_ticks)
    e.uc.mem_write(BUSL, bytes(4 * N))
    e.uc.mem_write(BUSR, bytes(4 * N))
    if e.call("clkout_block", BUFSET, N, count=2_000_000) is None:
        pass
    r0 = e.uc.reg_read(__import__("unicorn.arm_const", fromlist=["x"]).UC_ARM_REG_R0)
    if r0:
        e.call("clkout_mix", BUSL, BUSR, N, count=2_000_000)
    l = struct.unpack(f"<{N}f", e.uc.mem_read(BUSL, 4 * N))
    r = struct.unpack(f"<{N}f", e.uc.mem_read(BUSR, 4 * N))
    return r0, l, r


def opts(ppq_step, level_pct=None):
    opt(7, (ppq_step + .5) / 7.0)
    opt(8, 1.0 if level_pct is None else (level_pct / 100.0 - .05) / .95)


RATE = lambda bpm: bpm * 960.0 / (60.0 * 48000.0)

# ---- off by default
transport(True, 120.0)
r0, l, r = block(0)
check("CLK PPQ off (the default): no pulse, the bus untouched", r0 == 0 and not any(l) and not any(r), (r0,))
opts(0)
r0, l, r = block(100)
check("... set to 0 (off): nothing", r0 == 0 and not any(l))

# ---- positions
def stream(ppq_step, bpm, nblocks, start, level_pct=100, stamps=True, start_state=True):
    """nblocks of the pulse: the whole left channel as one list (and the right's equality), stamps advancing at the tempo"""
    opts(ppq_step, level_pct)
    transport(True, bpm)
    rate = RATE(bpm)
    out = []
    pos = start
    same = True
    for b in range(nblocks):
        r0, l, r = block(pos if stamps else None)
        same = same and l == r
        out.extend(l)
        pos += N * rate
    return out, same


def edges(x):
    return [i for i in range(len(x)) if x[i] and (i == 0 or not x[i - 1])]


def widths(x):
    w, c = [], 0
    for v in x:
        if v:
            c += 1
        elif c:
            w.append(c)
            c = 0
    return w


bpm = 120.0
rate = RATE(bpm)
for step, ppq in ((1, 1), (3, 4), (4, 8), (6, 24)):
    tpp = 960.0 / ppq
    x, same = stream(step, bpm, 400, 0.0)
    want = []
    k = 0
    while k * tpp / rate < 400 * N - 1:
        want.append(int(k * tpp / rate))
        k += 1
    got = edges(x)
    check(f"{ppq} PPQ at {bpm:g}: {len(want)} pulses in 400 blocks, each on its frame (+-3 frames after the first few while it settles, which the stamps' tick quantisation moves up to 14), the right channel the same",
          len(got) == len(want) and all(abs(g - w) <= (14 if i < 6 else 3) for i, (g, w) in enumerate(zip(got, want))) and same, ([g - w for g, w in zip(got, want)][:10], len(got), len(want)))
    wd = min(240, int(tpp / rate / 2))
    ws = widths(x)
    check(f"... each {wd} frames wide (the last may be cut off by the end)", ws[:-1] and all(abs(v - wd) <= 1 for v in ws[:-1]), (ws[:5], wd))

# ---- level
for pct in (100, 50, 5):
    x, _ = stream(1, 120.0, 400, 960.0 * 4, level_pct=pct)
    check(f"level {pct} %: a pulse is {pct / 100:g}", x and abs(max(x) - pct / 100.0) < 1e-4, max(x))

# ---- a locate backwards starts the count again; a pulse is not fired twice
x1, _ = stream(3, 120.0, 100, 5000.0)
x2, _ = stream(3, 120.0, 100, 1000.0)
exp = (100 * N * rate) / 240
check("a locate backwards: the pulses play again from there (counts as the span says, +-1)", abs(len(edges(x1)) - exp) <= 1 and abs(len(edges(x2)) - exp) <= 1, (len(edges(x1)), len(edges(x2)), exp))

# ---- a pulse straddling a block boundary is continuous
x, _ = stream(6, 120.0, 100, 1.0)
ws = widths(x)
check("pulses cut by a block boundary are whole (24 PPQ, every width the same)", ws[:-1] and len(set(ws[:-1])) <= 2, ws[:8])

# ---- tempo change in the middle: 120 -> 180 (same stamps position continuing)
xa, _ = stream(3, 120.0, 100, 2000.0)
xb, _ = stream(3, 180.0, 100, 2000.0 + 100 * N * RATE(120.0))
exp_b = (100 * N * RATE(180.0)) / 240
check("tempo 180: the pulses follow it (4 PPQ)", abs(len(edges(xb)) - exp_b) <= 1, (len(edges(xb)), exp_b))

# ---- no stamps: the running flag, from the start; stopped: nothing
transport(False, 120.0)
for b in range(3):
    block(None)
x, _ = stream(3, 120.0, 100, 0.0, stamps=False)
exp = (100 * N * rate) / 240
got = edges(x)
check("no sequence (stamps stand still), transport running: pulses from its start, the first on frame 0, at the tempo", got and got[0] == 0 and abs(len(got) - exp) <= 1.5, (got[:4], len(got), exp))
transport(False, 120.0)
r0 = 1
for b in range(3):
    r0, l, _ = block(None)
check("transport stopped: no pulse", r0 == 0 and not any(l))

print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
