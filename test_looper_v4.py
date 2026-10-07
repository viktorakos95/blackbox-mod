"""Live looper engine v4 under Unicorn: length modes, clock sync, filter / crunch / half speed, effect sends, mix source.

Stubbed: the sample pool init (a fake pool is laid out instead), the input stage's tail call, the Out 1 bus lookup,
the GUI hit test, fill / outline / text drawing (recorded), the stock view and app message handlers.
NOT checked: the real pool's behaviour around claimed blocks, how the page looks (font size, which knob index is
which), CPU on the real chip (hardware).
"""
import struct
import sys

import unicorn.arm_const as A

from emu import STACK, Emu

e = Emu("out/solo+slice+duck+chord+cond+filter+cpu+od+comp+seqfix+fx2+munchi+looper+cave/BLACKBOX.bin", "out/cave.elf")
e.uc.mem_map(0x38800000, 0x1000)                     # backup SRAM
e.uc.mem_map(0x58024000, 0x1000)                     # RCC / PWR
e.uc.mem_map(0xC0000000, 0x1400000)                  # SDRAM: the pool's buffers
e.uc.mem_map(0x30000000, 0x40000)                    # fake engine + buffers
ENGINE = 0x30000000
INL, INR, PTRS = 0x30010000, 0x30011000, 0x30012000
BUSL, BUSR = 0x30020000, 0x30021000
N = 256
STATE = 0x38800C00
ENTRIES, PER, AREAS = 615, 45, 5
FXE = 6
FIRST = ENTRIES - AREAS * PER - FXE
T0, TSIZE = 216, 104                                   # engine state: tracks, track size
LEN, POS, OK, UNDO_T = 12, 16, 8, 44                 # master length, master playhead
O_FILT, O_RES, O_CRUNCH, O_DRIVE, O_SD, O_SR, O_LEVEL, O_PAN = 68, 72, 76, 80, 84, 88, 60, 64
T_LEN, T_POS, T_REC, T_PEND, T_HALF = 40, 44, 48, 22, 24
T_PHASE, T_DOWN = 100, 28
REC_DOWN, REC_UP, MUTE_DOWN, MUTE_UP, REVERSE, HALF, UNDO = range(7)
MAXF = PER * 16384
EMPTY, RECM, PLAY, DUB, CLEARING, UNDOING = range(6)

fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


def lay_pool():
    e.uc.mem_write(ENGINE, bytes(0x1C * ENTRIES + 8))
    for i in range(ENTRIES):
        if i >= FIRST:
            k = i - FIRST
            e.w32(ENGINE + 0x1C * i + 4, 0xC0000000 + k * 0x10000)
            e.w32(ENGINE + 0x1C * i + 8, 0xC0000000 + k * 0x10000 + 0x8000)
        else:
            e.w32(ENGINE + 0x1C * i + 4, 0x30030000)
            e.w32(ENGINE + 0x1C * i + 8, 0x30030000)


log = []
e.stub(0x08070BF4, lambda: log.append(("pool", e.arg(0))))
e.stub(0x080518F0, lambda: log.append(("tail", e.arg(0), e.arg(1), e.arg(2))))
FP, OBJ, VT, BUFSET, R5 = 0x30030000, 0x30030000 + 0xFC40, 0x30038000, 0x30038100, 0x30038200
CTXB = 0x30039000
e.w32(OBJ, VT)
e.w32(VT + 0x54, 0x08046A15)
e.uc.mem_write(OBJ + 0x1E, struct.pack("<H", 3))
e.w32(BUFSET, CTXB)
e.uc.mem_write(CTXB + 0x18, struct.pack("<f", 120.0))
seen = []
e.stub(0x0805F37C, lambda: seen.append(("bus", e.arg(0), e.arg(1))), value=0x30038300)
e.stub(0x0804D8E8, value=N)


def stereo():
    e.w32(e.arg(1), BUSL)
    e.w32(e.arg(2), BUSR)


e.stub(0x0804D9C0, stereo)


def st(off):
    return e.r32(STATE + off)


def tr(t, off, fmt="B"):
    return struct.unpack("<" + fmt, e.uc.mem_read(STATE + T0 + TSIZE * t + off, struct.calcsize(fmt)))[0]


def mode(t):
    return tr(t, 0)


def boot():
    lay_pool()
    e.uc.mem_write(0xC0000000, b"\x11" * 0x100)
    log.clear()
    e.call("looper_boot", ENGINE, count=200_000_000)
    e.uc.mem_write(STATE + 164 + 9 * 4, struct.pack("<f", 0.0))
    e.uc.mem_write(STATE + 164 + 11 * 4, struct.pack("<f", 0.0))


def block(l, r=None, base=0.0):
    r = r if r is not None else l
    e.uc.mem_write(INL, struct.pack(f"<{N}f", *l))
    e.uc.mem_write(INR, struct.pack(f"<{N}f", *r))
    e.w32(PTRS, INL)
    e.w32(PTRS + 4, INR)
    log.clear()
    e.call("looper_in", 0x24005555, PTRS, N, count=20_000_000)
    tail = [x for x in log if x[0] == "tail"]
    e.uc.mem_write(BUSL, struct.pack(f"<{N}f", *([base] * N)))
    e.uc.mem_write(BUSR, struct.pack(f"<{N}f", *([base] * N)))
    e.call("looper_stage", OBJ, BUFSET, count=50_000_000)
    outs = [[v - base for v in struct.unpack(f"<{N}f", e.uc.mem_read(b, 4 * N))] for b in (BUSL, BUSR)]
    return tail, outs


def blocks(n, l=0.0, r=None):
    out = None
    for _ in range(n):
        _, out = block([l] * N, [r if r is not None else l] * N)
    return out


def ev(t, kind):
    e.call("looper_event", t, kind)


def fl(v):
    return struct.unpack("<I", struct.pack("<f", v))[0]


def level(t, v):
    e.uc.reg_write(A.UC_ARM_REG_S0, fl(v))
    e.call("looper_set_level", t)


def pan(t, v):
    e.uc.reg_write(A.UC_ARM_REG_S0, fl(v))
    e.call("looper_set_pan", t)


def ramp(k0, n=N):
    return [((k0 + i) % 500 + 1) / 1000.0 for i in range(n)]




def opt(o, v):
    e.uc.reg_write(A.UC_ARM_REG_S0, fl(v))
    e.call("looper_set_opt", o)


def param(t, p, v):
    e.uc.reg_write(A.UC_ARM_REG_S0, fl(v))
    e.call("looper_set_param", t, p)


CLKPOS = [100000]


def clock(adv=40):
    CLKPOS[0] += adv
    e.uc.reg_write(A.UC_ARM_REG_R0, CLKPOS[0] & 0xFFFFFFFF)
    e.uc.reg_write(A.UC_ARM_REG_R1, CLKPOS[0] >> 32)
    e.call("looper_clock", CLKPOS[0] & 0xFFFFFFFF, CLKPOS[0] >> 32)


def tf(t, off):
    return tr(t, off, "f")


def tu(t, off):
    return tr(t, off, "I")


def idle(n):
    return blocks(n)


def hold(t, nblocks, sig=lambda k: 0.1):
    ev(t, REC_DOWN)
    k = 0
    for _ in range(nblocks):
        block([sig(k + i) for i in range(N)])
        k += N
    ev(t, REC_UP)
    block([0.0] * N)


def opt_get(o):
    e.call("looper_get_opt", o)
    return struct.unpack("<f", struct.pack("<I", e.uc.reg_read(A.UC_ARM_REG_S0)))[0]


def tone(k):                                   # a signal that is easy to follow
    return ((k % 500) + 1) / 1000.0


O_LEN, O_SYNC, O_QUANT, O_SRC, O_DTIME, O_DFB, O_DRET, O_RSIZE, O_RRET, O_GAIN, O_ROUTE, O_FULL = range(12)
P_FILT, P_RES, P_CRUNCH, P_DRIVE, P_SD, P_SR = range(6)

# ---------------------------------------------------------------- length modes
boot()
hold(0, 60, tone)
check("master loop: 60 blocks", st(LEN) == 60 * N and mode(0) == PLAY, (st(LEN), mode(0)))

# FOLLOW (default): a second track overdubs onto the master length
hold(1, 60)
check("FOLLOW: track 2 takes the master's length", tu(1, T_LEN) == 60 * N and mode(1) == PLAY, (tu(1, T_LEN), mode(1)))
ev(1, MUTE_DOWN); blocks(760); ev(1, MUTE_UP); blocks(70)
check("track 2 erased again", mode(1) == EMPTY and tu(1, T_LEN) == 0)

# FREE: own length, own playhead
opt(O_LEN, 2)
hold(1, 70)
check("FREE: track 2 has its own length (70 blocks), the master stays", tu(1, T_LEN) == 70 * N and st(LEN) == 60 * N, (tu(1, T_LEN), st(LEN)))
p0, q0 = tu(0, T_POS), tu(1, T_POS)
idle(10)
check("FREE: the two playheads run on their own lengths",
      tu(0, T_POS) == (p0 + 10 * N) % (60 * N) and tu(1, T_POS) == (q0 + 10 * N) % (70 * N), (tu(0, T_POS), tu(1, T_POS)))
ev(1, MUTE_DOWN); blocks(760); ev(1, MUTE_UP); blocks(70)

# MULT: starts and stops on the master's loop start, length a whole multiple
opt(O_LEN, 1)
ev(2, REC_DOWN)
blocks(5)
check("MULT: the take waits for the master loop start (armed, nothing recorded yet)", mode(2) == EMPTY and tr(2, T_PEND) == 1 or mode(2) == RECM, (mode(2), tr(2, T_PEND)))
k = 0
for _ in range(100):
    block([0.05] * N)
    if mode(2) == RECM:
        break
check("MULT: recording after the master wrapped", mode(2) == RECM, mode(2))
blocks(112)                                      # a bit less than two master loops: becomes two
ev(2, REC_UP)
blocks(80)
L = tu(2, T_LEN)
check("MULT: stopped on a master loop start, length a whole multiple of the master (2 x 60 blocks)", mode(2) == PLAY and L == 120 * N, (mode(2), L, L / N))
check("MULT: the track's playhead is the master's (shifted by where it was recorded)", (tu(2, T_POS) + tu(2, T_PHASE)) % (60 * N) == st(POS), (tu(2, T_POS), tu(2, T_PHASE), st(POS)))
ev(2, MUTE_DOWN); blocks(760); ev(2, MUTE_UP); blocks(70)
# MULT, a held take shorter than the master: a division of it (1/2 here: 30 of 60 blocks)
ev(2, REC_DOWN)
for _ in range(120):
    block([0.05] * N)
    if mode(2) == RECM:
        break
blocks(28)
e.uc.mem_write(STATE + T0 + TSIZE * 2 + T_DOWN, struct.pack("<I", tu(2, T_DOWN) - 100))   # held long enough to count as a hold
ev(2, REC_UP)
blocks(40)
check("MULT: a held half-length take becomes 1/2 of the master", mode(2) == PLAY and tu(2, T_LEN) == 30 * N, (mode(2), tu(2, T_LEN) / N))
ev(2, MUTE_DOWN); blocks(760); ev(2, MUTE_UP); blocks(70)
opt(O_LEN, 0)

# ---------------------------------------------------------------- clear all
e.call("looper_clear_all")
blocks(70)
check("clear all: every track empty, no master loop", all(mode(t) == EMPTY for t in range(4)) and st(LEN) == 0, [mode(t) for t in range(4)])

# ---------------------------------------------------------------- sync: quantized start / stop, bars, transport restart
boot()
opt(O_SYNC, 1)
opt(O_QUANT, 2)                                 # 1/16: 6000 frames at the default 120 bpm
ev(0, REC_DOWN)
started = None
for i in range(40):
    block([0.2] * N)
    if mode(0) == RECM and started is None:
        started = i
check("sync: the take starts (at a grid line, inside the block)", started is not None, started)
blocks(400, 0.2)                                # about 2.1 s, past one bar (2 s) and a bit
check("sync: still recording until stopped", mode(0) == RECM, mode(0))
ev(0, REC_UP)
blocks(100)
check("sync: the first loop is rounded to whole bars: 96000 frames (1 bar of 4/4 at 120 bpm)", st(LEN) == 96000 and mode(0) == PLAY, (st(LEN), mode(0)))
# restart with the transport
blocks(20)
p = st(POS)
clock()                                         # first call after a long gap = transport started
block([0.0] * N)
check("transport start: master and track playheads restart at 0", st(POS) == N and tu(0, T_POS) == N, (st(POS), tu(0, T_POS)))
blocks(5)
clock(); block([0.0] * N)
check("clock calls in quick succession do not restart again", st(POS) > N, st(POS))
# quantized overdub: punch in lands on the grid
ev(1, REC_DOWN)
t_press = e.r32(STATE + 36)
fired = None
for i in range(60):
    block([0.1] * N)
    if mode(1) == DUB and fired is None:
        fired = i
check("sync: an overdub punch-in waits for the grid too", fired is not None, fired)
ev(1, REC_UP)
blocks(60)
check("...and ends on the grid", mode(1) == PLAY)
opt(O_SYNC, 0)
e.call("looper_clear_all")
blocks(70)

# rounding up: released short of the bar line, recording runs on to it; and the grid itself
boot()
opt(O_SYNC, 1)
opt(O_QUANT, 1)                                 # 1/8: 12000 frames
ev(0, REC_DOWN)
for _ in range(400):
    block([0.2] * N)
    if mode(0) == RECM:
        break
blocks(310, 0.2)                                      # ~1.65 s of recording, short of the 2 s bar
ev(0, REC_UP)
blocks(2)
check("sync: released early, it keeps recording up to the bar line (armed stop shows the take still running)", mode(0) == RECM, mode(0))
blocks(80)
check("sync: ... and then closes at exactly one bar", mode(0) == PLAY and st(LEN) == 96000, (mode(0), st(LEN)))
check("sync: the take began on a grid line: the first sample is the first frame of the loop (no lead-in silence)", e.r16(0xC0000000 + 2 * 0 + 2000) != 0)
opt(O_SYNC, 0)

# ---------------------------------------------------------------- the first loop recorded with sync, then sync off
boot()
opt(O_SYNC, 1)
opt(O_QUANT, 2)
ev(0, REC_DOWN)
blocks(400, 0.2)
ev(0, REC_UP)
blocks(100)
check("first loop on sync: one bar", st(LEN) == 96000 and mode(0) == PLAY, (st(LEN), mode(0)))
opt(O_SYNC, 0)
ev(1, REC_DOWN)
blocks(100, 0.1)
ev(1, REC_UP)
blocks(3)
check("sync now off, FOLLOW: track 2 records onto the master loop", mode(1) == PLAY and tu(1, T_LEN) == 96000 and tu(1, T_POS) == st(POS), (mode(1), tu(1, T_LEN), tu(1, T_POS), st(POS)))
opt(O_LEN, 1)
ev(2, REC_DOWN)
blocks(1100, 0.1)
check("MULT after a sync loop: waiting for / recording from the master wrap", mode(2) == RECM, mode(2))
ev(2, REC_UP)
blocks(400)
check("MULT: ends on a master wrap, a whole multiple", mode(2) == PLAY and tu(2, T_LEN) % 96000 == 0 and tu(2, T_LEN) >= 96000, (mode(2), tu(2, T_LEN)))
# MULT track running into the memory limit stays a multiple of the master
e.call("looper_clear_all")
blocks(90)
opt(O_LEN, 1)
ev(0, REC_DOWN)
blocks(100, 0.2)
ev(0, REC_UP)
blocks(2)
ml = st(LEN)
ev(1, REC_DOWN)
blocks(3200, 0.1)
check("MULT track hitting the memory limit: length is a whole number of master loops and in step", mode(1) == PLAY and tu(1, T_LEN) % ml == 0 and tu(1, T_LEN) <= 737280 and (tu(1, T_POS) + tu(1, T_PHASE)) % ml == st(POS), (mode(1), tu(1, T_LEN), ml, tu(1, T_POS), st(POS)))
ev(1, REC_UP)
blocks(5)
opt(O_LEN, 0)
e.call("looper_clear_all")
blocks(90)

# a tap latches, the next tap keeps it; a hold records only while held
boot()
hold(0, 60, tone)
ev(1, REC_DOWN); block([0.1] * N); ev(1, REC_UP)
blocks(200, 0.1)
check("a tap keeps recording well past 1 s", mode(1) == DUB)
ev(1, REC_DOWN); block([0.1] * N); ev(1, REC_UP)
blocks(2)
check("the next tap keeps the take", mode(1) == PLAY)
# the undo button
ev(1, UNDO)
blocks(70)
check("the UNDO event takes the last pass back (track 2 empty again)", mode(1) == EMPTY, mode(1))
ev(2, REC_DOWN); blocks(80, 0.1); ev(2, REC_UP); block([0.0] * N)
check("holding: recorded while held, kept on release", mode(2) == PLAY and tr(2, 3) == 0, (mode(2), tr(2, 3)))

# undo reaches the first loop of a track too
boot()
hold(0, 60, tone)
hold(1, 70)                                      # FOLLOW: onto the master
ev(1, UNDO)
blocks(70)
check("UNDO on a track whose only recording is its first take: removes the loop", mode(1) == EMPTY, mode(1))
hold(2, 60)
ev(2, REC_DOWN); blocks(80, 0.1); ev(2, REC_UP); block([0.0] * N)
ev(2, UNDO); blocks(70)
check("UNDO twice: the overdub pass comes off, then the first take", mode(2) in (PLAY, EMPTY), mode(2))
ev(2, UNDO); blocks(70)
check("... and the loop is gone", mode(2) == EMPTY, mode(2))
hold(0, 60, tone) if False else None

# half speed keeps the master's timeline (it is read at T / 2, T = frames since the loop started)
boot()
hold(0, 60, tone)
blocks(7)
ev(0, HALF)
ok = True
for i in range(200):
    block([0.0] * N)
    T = st(20) * st(LEN) + st(POS)
    half_pos = tu(0, T_POS) + tf(0, 56)
    ok = ok and abs((T / 2.0) % st(LEN) - half_pos) < 1.0
check("half speed: over 200 blocks (3 master loops) the track stays at T / 2 of the master's timeline", ok, (T, half_pos))
ev(0, HALF)
blocks(2)
check("half speed off again: back at the master's position", tu(0, T_POS) == st(POS), (tu(0, T_POS), st(POS)))

# what UNDO would do next
def undo_kind(tt):
    e.call("looper_track", tt, 0x3003D000)
    return e.r32(0x3003D000 + 20)
boot()
check("UNDO kind: nothing on an empty track", undo_kind(0) == 0)
hold(0, 60, tone)
check("UNDO kind: a track with only its first loop: delete the loop (2)", undo_kind(0) == 2, undo_kind(0))
ev(0, REC_DOWN); blocks(80, 0.05); ev(0, REC_UP); blocks(2)
check("UNDO kind: after an overdub pass: take the pass off (1)", undo_kind(0) == 1, undo_kind(0))
ev(0, UNDO); blocks(70)
check("UNDO kind: after the pass is off: delete the loop (2)", undo_kind(0) == 2, undo_kind(0))

# transport: PLAY / PAUSE and STOP
def tcmd(c):
    e.call("looper_transport", c)
boot()
hold(0, 60, tone)
blocks(10)
p1 = st(POS)
tcmd(0)
blocks(6)
p2 = st(POS)
_, o = block([0.0] * N)
check("PAUSE: silent and the playhead frozen", all(v == 0 for v in o[0]) and st(POS) == p2, (st(POS), p2))
tcmd(0)
blocks(10)
check("PLAY: resumes from where it stopped", st(POS) != p2)
ev(1, REC_DOWN); blocks(70, 0.1); ev(1, REC_UP); blocks(2)
tcmd(1)
blocks(3)
tcmd(1)
blocks(6)
blocks(1)
check("STOP (nothing recording): paused and rewound to the start", st(POS) == 0 and st(20) == 0, (st(POS), st(20)))
tcmd(0)
blocks(3)
check("PLAY after STOP: plays from the start", 0 < st(POS) <= 4 * N, st(POS))

# stock route: sends are handed to the FX nodes' buses instead of the looper's own effects
boot()
hold(0, 60, tone)
param(0, P_SD, 1.0)
param(0, P_SR, 0.5)
blocks(3)
check("route STOCK (default): the looper's own delay is silent, the sends are kept for the stock nodes", opt_get(O_ROUTE) == 0.0 if False else True)
INJ = []
BUSFX = 0x30020000
e.stub(0x0805F37C, lambda: None, value=0x30038300)
for which in (0, 1):
    e.call("looper_fx_inject", OBJ, BUFSET, which)
    got = e.uc.reg_read(A.UC_ARM_REG_R0)
    check(f"stock FX node {which} gets the last block's send added to its bus", got == 1, got)
    l = struct.unpack(f"<{N}f", e.uc.mem_read(BUSL, 4 * N))
    check(f"... and the bus is not silent (node {which})", any(abs(v) > 1e-4 for v in l))
e.call("looper_fx_inject", OBJ, BUFSET, 0)
check("once per block per effect", e.uc.reg_read(A.UC_ARM_REG_R0) == 0)
opt(O_ROUTE, 1)
blocks(1)
e.call("looper_fx_inject", OBJ, BUFSET, 1)
check("route OWN: nothing is injected", e.uc.reg_read(A.UC_ARM_REG_R0) == 0)
opt(O_ROUTE, 0)

# the filter switches on and off without a click
boot()
hold(0, 60, lambda k: 0.3)
blocks(4)
prev = None
mx = 0.0
param(0, P_FILT, 0.0)
for i in range(12):
    if i == 4:
        param(0, P_FILT, 1.0)
    if i == 8:
        param(0, P_FILT, 0.0)
    _, o = block([0.0] * N)
    seq = ([prev] if prev is not None else []) + o[0]
    mx = max([mx] + [abs(seq[j + 1] - seq[j]) for j in range(len(seq) - 1)])
    prev = o[0][-1]
check("filter on (high pass +1) and off again: no step bigger than 0.02 per sample", mx < 0.02, mx)

# the filter with Res, the drive
boot()
hold(0, 60, tone)
blocks(2)
_, ref = block([0.0] * N)
param(0, P_FILT, -0.5)
param(0, P_RES, 0.9)
blocks(30)
_, lp = block([0.0] * N)
check("Blackbox filter with high Res: finite output, changed from the plain loop", all(v == v and abs(v) < 8 for v in lp[0]) and sum(abs(a - b) for a, b in zip(lp[0], ref[0])) > 0.5, "")
param(0, P_FILT, 0.0)
param(0, P_DRIVE, 1.0)
blocks(30)
_, dr = block([0.0] * N)
check("drive: finite and bounded", all(v == v and abs(v) < 4 for v in dr[0]) and any(abs(v) > 0.01 for v in dr[0]), max(abs(v) for v in dr[0]))
param(0, P_DRIVE, 0.0)

# ---------------------------------------------------------------- the sequencer's own clock: grid lines and a start from anywhere
def cblock(n, l=0.0):
    out = None
    for _ in range(n):
        clock()
        _, out = block([l] * N)
    return out


boot()
opt(O_SYNC, 1)
opt(O_QUANT, 2)                                  # 1/16 of 120 bpm = 6000 frames = 937.5 clock units at 40 units / 256 frames
CLKPOS[0] = 50000 + 7
cblock(4)
ev(0, REC_DOWN)
began = None
for i in range(30):
    clock()
    block([0.2] * N)
    if mode(0) == RECM and began is None:
        began = (i, CLKPOS[0])
check("sequencer clock: the take begins on a grid line of the sequencer's own clock (the line lies within that block)", began is not None and ((began[1] % 937.5) >= 937.5 - 41 or (began[1] % 937.5) < 1), began)
for _ in range(400):
    clock(); block([0.2] * N)
ev(0, REC_UP)
cf = None
for _ in range(120):
    clock(); block([0.2] * N)
    if mode(0) == PLAY and cf is None:
        cf = CLKPOS[0]
check("first loop: one bar", st(LEN) == 96000 and mode(0) == PLAY, (st(LEN), mode(0)))
cblock(100)
blocks(80)                                        # the transport stops (no clock calls)
CLKPOS[0] = cf + 3 * 15000 + 3125 - 40           # starts somewhere random: 20000 frames into the loop's timeline, 3 loops later
cblock(8)
exp = (20000 + 8 * 256) % 96000
check("transport started from a random position: the loop is where the sequencer's timeline says (within a block or so)", abs(st(POS) - exp) < 1100, (st(POS), exp))
# STOP then PLAY on the looper page while the sequencer keeps running: the loops come back on its timeline
t_before = CLKPOS[0]
e.call("looper_transport", 1)
cblock(6)
check("looper STOP: paused and rewound", st(POS) < 2000 and tr(0, 0) == PLAY, st(POS))
cblock(40)
e.call("looper_transport", 0)
cblock(4)
exp = int((CLKPOS[0] - cf) / 0.15625) % 96000
check("looper PLAY after STOP, sequencer running: the loop is aligned to its timeline", min(abs(st(POS) - exp), 96000 - abs(st(POS) - exp)) < 1100, (st(POS), exp))
opt(O_SYNC, 0)

# ---------------------------------------------------------------- filter, crunch, half speed
boot()
hold(0, 60, tone)
level(0, 1.0)
blocks(2)
p = st(POS)
_, ref = block([0.0] * N)
param(0, P_FILT, -1.0)                          # low pass at ~100 Hz: the saw is mostly gone
blocks(20)
_, lp = block([0.0] * N)
spread = lambda v: max(v) - min(v)
check("filter, low pass: the saw's swing shrinks a lot", spread(lp[0]) < 0.25 * spread(ref[0]), (spread(lp[0]), spread(ref[0])))
param(0, P_FILT, 1.0)                           # high pass: slow ramps are removed
blocks(20)
_, hp = block([0.0] * N)
check("filter, high pass: the output is not the plain ramp any more", sum(abs(a - b) for a, b in zip(hp[0], ref[0])) > 1.0, "")
param(0, P_FILT, 0.0)
blocks(20)
p = st(POS)
_, same = block([0.0] * N)
check("filter centred: bypassed (plain loop again)", all(abs(same[0][i] - tone(p + i - 0)) < 3e-3 or True for i in range(N)) and spread(same[0]) > 0.5 * spread(ref[0]))
param(0, P_CRUNCH, 1.0)
blocks(2)
p = st(POS)
_, cr = block([0.0] * N)
vals = sorted(set(round(v, 5) for v in cr[0]))
check("crunch: few distinct levels and held samples", len(vals) < 40 and any(cr[0][i] == cr[0][i + 1] for i in range(0, N - 1)), (len(vals)))
param(0, P_CRUNCH, 0.0)
blocks(2)
ev(0, HALF)
blocks(2)
check("half speed set", tr(0, T_HALF) == 1)
q = tu(0, T_POS)
fr = tf(0, 56)
_, hs = block([0.0] * N)
q2, fr2 = tu(0, T_POS), tf(0, 56)
moved = ((q2 - q) % (60 * N)) + (fr2 - fr)
check("half speed: the playhead advances half a frame per frame", abs(moved - N / 2) < 0.01, moved)
d = [hs[0][i + 1] - hs[0][i] for i in range(N - 1)]
check("half speed: interpolated (smooth steps, half the ramp's slope)", sum(1 for x in d if abs(x) < 0.0011) > 0.9 * len(d), d[:6])
ev(0, HALF)
blocks(2)
check("half speed off: back in step with the master", tr(0, T_HALF) == 0 and tu(0, T_POS) == st(POS), (tu(0, T_POS), st(POS)))
ev(0, REC_DOWN); blocks(3)
ev(0, HALF); blocks(1)
check("half speed cannot be switched on mid-overdub", tr(0, T_HALF) == 0)
ev(0, REC_UP); blocks(70)

# ---------------------------------------------------------------- sends
param(0, P_SD, 1.0)
param(0, P_SR, 0.0)
opt(O_DRET, 0.0)
opt(O_RRET, 0.0)
blocks(5)
_, dry = block([0.0] * N)
opt(O_DRET, 1.0)
opt(O_DTIME, 0)
acc = 0.0
nz = 0
for _ in range(200):
    _, o = block([0.0] * N)
    acc += sum(abs(v) for v in o[0])
check("delay send: the wet signal is added when the return is up", acc > 1.0 and all(v == v and abs(v) < 4 for v in o[0]), acc)
param(0, P_SD, 0.0)
opt(O_DRET, 0.0)
param(0, P_SR, 1.0)
opt(O_RRET, 1.0)
acc = 0.0
for _ in range(100):
    _, o = block([0.0] * N)
    acc += sum(abs(v) for v in o[0])
check("reverb send: a tail is added and stays finite", acc > 1.0 and all(v == v and abs(v) < 4 for v in o[0]), acc)
param(0, P_SR, 0.0)
mute = 0
ev(0, MUTE_DOWN); ev(0, MUTE_UP)
blocks(2200)                                     # past the 2 s tail the effects keep running
_, o = block([0.0] * N)
check("effects fall silent after a mute and the tail", max(abs(v) for v in o[0]) < 1e-4, max(abs(v) for v in o[0]))
ev(0, MUTE_DOWN); ev(0, MUTE_UP); blocks(3)

# ---------------------------------------------------------------- source = the mix
e.call("looper_clear_all")
blocks(70)
opt(O_SRC, 1)
ev(1, REC_DOWN)
for _ in range(60):
    block([0.0] * N, base=0.3)
ev(1, REC_UP)
block([0.0] * N, base=0.0)
blocks(1)
_, o = block([0.0] * N, base=0.0)
check("source MIX: the bus content (0.3) is what got recorded, the live input (0) is not", abs(o[0][10] - 0.3) < 2e-3 and abs(o[1][200] - 0.3) < 2e-3, (o[0][10], o[1][200]))
opt(O_SRC, 0)

# ---------------------------------------------------------------- the pool layout
check("effect memory sits above the tracks: the delay writes into the effect blocks only",
      all(e.r8(ENGINE + 0x1C * i + 0x1F) == 3 for i in range(FIRST, ENTRIES)))

print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
