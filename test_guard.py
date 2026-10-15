"""The looper's guard (looper.c: guard_decide / guard_fit / looper_block_done) under Unicorn.

The emulator's cycle counter does not run: the test writes the block period, the time used before the tracks and the whole block's
load at its end (as cpu.c reports it). Checked: with room nothing changes; a block near the limit makes one track fade out over
~0.75 s (track 3 first, the others untouched) and then it is muted (shown muted); one at a time with a pause; it never comes back by
itself; the player unmutes it and it plays, and it is the last to go again; no time left in a block cuts the tracks in that block (a
ramp to 0, then muted).
NOT checked: the real cycle counts on the unit (the model's constants are fitted to emulator instruction counts).

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
e.uc.mem_map(0xE0001000, 0x1000)                     # DWT cycle counter
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
T0, TSIZE = 224, 136                                   # engine state: tracks, track size
LEN, POS, OK, UNDO_T = 12, 16, 8, 44                 # master length, master playhead
O_FILT, O_RES, O_CRUNCH, O_DRIVE, O_SD, O_SR, O_LEVEL, O_PAN = 68, 72, 76, 80, 84, 88, 60, 64
T_LEN, T_POS, T_REC, T_PEND, T_HALF = 40, 44, 48, 22, 24
T_PHASE, T_DOWN = 132, 28
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
    e.uc.mem_write(STATE + 164 + 1 * 4, struct.pack("<f", 0.0))               # SYNC off and QUANT 1/16: what the checks assume (the defaults are SYNC on, 1 BAR)
    e.uc.mem_write(STATE + 164 + 2 * 4, struct.pack("<f", 3.0))


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


def flat2(n):
    out = []
    for _ in range(n):
        _, o = block([0.0] * N)
        out += o[0]
    return out


def tone(k):                                   # a signal that is easy to follow
    return ((k % 500) + 1) / 1000.0


O_LEN, O_SYNC, O_QUANT, O_SRC, O_DTIME, O_DFB, O_DRET, O_RSIZE, O_RRET, O_GAIN, O_ROUTE, O_FULL, O_HW, O_MON, O_PITCH = range(15)
P_FILT, P_RES, P_CRUNCH, P_DRIVE, P_SD, P_SR, P_STAB, P_RPT, P_SPEED, P_DROP, P_TRIM, P_STUT, P_SCRM, P_PTCH = range(14)



import subprocess
def dsp_off(field):
    src = '#include "src/looper.c"\nchar o_x[__builtin_offsetof(struct dsp, %s)];\n' % field
    out = subprocess.run(["arm-none-eabi-gcc", "-I.", "-Isrc", "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16", "-ffreestanding", "-S", "-x", "c", "-", "-o", "-"],
                         input=src, capture_output=True, text=True, check=True).stdout.splitlines()
    return int([l.split()[1] for l in out if "o_x" in l and ".comm" in l or l.strip().startswith(".space")][-1].split(",")[1 if ".comm" in l else 0]) if False else \
        int([l.split()[1] for l in out if l.strip().startswith(".space")][0])
boot()
opt(O_PITCH, 1)
hold(0, 60, tone)
for tt in (1, 2, 3):
    hold(tt, 40 + 7 * tt, lambda k, tt=tt: ((k * (tt + 2)) % 700 - 350) / 900.0)
blocks(10)
for t in range(4):
    if mode(t) == DUB:                                 # (the hold above left it overdubbing: REC ends that)
        ev(t, REC_DOWN)
        ev(t, REC_UP)
blocks(10)
check("setup: four tracks play", [mode(t) for t in range(4)] == [PLAY] * 4, [mode(t) for t in range(4)])
for t in range(4):
    for q, v in ((P_FILT, -0.4), (P_RES, 0.7), (P_DRIVE, 0.5)):
        param(t, q, v)
FXB = e.r32(ENGINE + 0x1C * (FIRST + 5 * PER) + 4) & 0xFFFFFFFF
def g_at(f):
    return FXB + dsp_off("gd." + f)
G_PER, G_REST, G_FX, G_CPI, G_GM, G_FADING, G_FO, G_TQ = (g_at(f) for f in ("per", "rest", "fx", "cpi", "gm", "fading", "fo", "tq"))
PERC = 2_128_000                                       # a 256-frame block at 399 MHz
MUTED = 1                                              # struct track: mode, muted, ...
def setf(a, v):
    e.uc.mem_write(a, struct.pack("<f", v))
def run_at(pre, whole, nb=1):
    """nb blocks: pre = how full the block is before the tracks, whole = at its end (cpu.c's figure)"""
    outs = []
    for _ in range(nb):
        setf(G_PER, PERC)
        setf(G_REST, 0.0)
        setf(G_FX, 0.0)
        dwt = e.r32(0xE0001004) & 0xFFFFFFFF
        wake = (dwt - int(pre * PERC)) & 0xFFFFFFFF
        e.w32(0x2405FFD4, wake)
        _, o = block([0.0] * N)
        outs.append(o)
        e.call("looper_block_done", (wake + int(whole * PERC)) & 0xFFFFFFFF)
    return outs
def muted():
    return [tr(t, MUTED) for t in range(4)]
def gm():
    return list(e.uc.mem_read(G_GM, 4))
def fading():
    return list(e.uc.mem_read(G_FADING, 4))
def fo():
    return [round(v, 3) for v in struct.unpack("<4f", e.uc.mem_read(G_FO, 16))]
def tq():
    return list(e.uc.mem_read(G_TQ, 4))
def rms(o):
    return (sum(v * v for v in o[0]) / len(o[0])) ** 0.5
blocks(20)
e.call("looper_page_set_leader", 2)                    # DRONE-8's page was opened last: the looper gives way first (the order until now)
o = run_at(0.10, 0.80, 400)
check("room (whole block 80 %): nothing fades, nothing muted, every track as set", muted() == [0] * 4 and fading() == [0] * 4 and tq() == [0] * 4,
      (muted(), fading(), tq()))
base = rms(o[-1])
check("room: the tracks sound", base > 0.01, base)
o = run_at(0.10, 0.95, 190)                            # a half-second window with a block at 95 %
check("near the limit: track 3 starts to fade out, the others stay as set", fading() == [0, 0, 1, 0] and tq() == [0] * 4 and muted() == [0] * 4,
      (fading(), tq(), muted()))
f0 = fo()[2]
o = run_at(0.10, 0.95, 100)
check("... slowly (~0.75 s)", 0.3 < fo()[2] - f0 < 0.45 and muted()[2] == 0, (f0, fo()[2], muted()))
o = run_at(0.10, 0.95, 200)
check("... then muted (the page shows it), and only that one", muted() == [0, 0, 1, 0] and gm() == [0, 0, 1, 0] and fading() == [0] * 4,
      (muted(), gm(), fading()))
check("... the other three sound", rms(o[-1]) > 0.3 * base, (rms(o[-1]), base))
o = run_at(0.10, 0.95, 300)
check("still near the limit: the next one (track 2) only after a pause", fading()[1] == 0 and muted()[1] == 0, (fading(), muted()))
o = run_at(0.10, 0.95, 500)
check("... then track 2", muted() == [0, 1, 1, 0], muted())
for _ in range(800):                                   # the next one (track 1) starts to fade ...
    run_at(0.10, 0.95, 1)
    if fading()[0]:
        break
run_at(0.10, 0.95, 40)
check("(track 1 fading)", fading()[0] == 1 and muted()[0] == 0, (fading(), fo()))
o = run_at(0.10, 0.70, 700)                            # ... and the load goes down meanwhile
check("the load goes down during a fade: it turns back and fades in again", fading()[0] == 0 and muted()[0] == 0 and fo()[0] == 0.0, (fading(), muted(), fo()))
o = run_at(0.10, 0.70, 2000)
check("room again: nothing muted comes back by itself", muted() == [0, 1, 1, 0], muted())
ev(2, MUTE_DOWN)
ev(2, MUTE_UP)
o = run_at(0.10, 0.70, 60)
check("the player unmutes track 3: it plays", muted() == [0, 1, 0, 0] and gm() == [0, 1, 0, 0] and rms(o[-1]) > 0.3 * base, (muted(), gm(), rms(o[-1])))
o = run_at(0.10, 0.95, 1200)
check("near the limit again: the one just unmuted goes last (track 1 goes first)", muted()[0] == 1 and muted()[2] == 0, muted())
for t in range(4):                                     # all back on
    if tr(t, MUTED):
        ev(t, MUTE_DOWN)
        ev(t, MUTE_UP)
o = run_at(0.10, 0.70, 60)
check("(all unmuted)", muted() == [0] * 4, muted())
o = run_at(0.10, 0.70, 30)                             # the tracks' costs are measured (in the emulator: from the model)
e.uc.mem_write(G_CPI, struct.pack("<f", 1.0))
o = run_at(0.97, 0.99, 1)                              # a spike: no time left before the tracks
check("no time left in a block: the tracks are cut in it, and muted", muted() == [1] * 4 and gm() == [1] * 4, (muted(), gm()))
x = o[0][0]
peak = max(abs(v) for v in x)
check("... a ramp to 0 within the block, or nothing at all (no click from a cut mid-wave)", abs(x[-1]) < 0.01 * peak + 1e-5, (peak, x[-1]))
o = run_at(0.10, 0.70, 5)
check("... and silent after", max(max(abs(v) for v in b[0]) for b in o) < 1e-6, max(max(abs(v) for v in b[0]) for b in o))
# --- the leader: the engine whose page was opened last is protected longest
def r0s():
    v = e.uc.reg_read(A.UC_ARM_REG_R0) & 0xFFFFFFFF
    return v - (1 << 32) if v >= 1 << 31 else v
def left_pair():
    run_at(0.10, 0.70, 1)
    e.w32(0x2405FFD4, ((e.r32(0xE0001004) & 0xFFFFFFFF) - int(0.10 * PERC)) & 0xFFFFFFFF)
    e.call("looper_budget_left")
    base = r0s()
    e.call("looper_budget_left_samplr")
    return base, r0s()
for t in range(4):
    if tr(t, MUTED):
        ev(t, MUTE_DOWN)
        ev(t, MUTE_UP)
run_at(0.10, 0.70, 400)
e.call("looper_page_set_leader", 0)                    # the Looper page is up: the looper is protected longest (a fade only at 97 %)
o = run_at(0.10, 0.95, 400)
check("leader looper: 95 % in a block is no reason to fade a track", fading() == [0] * 4 and muted() == [0] * 4, (fading(), muted()))
o = run_at(0.10, 0.985, 400)
check("... 98.5 % is", muted() != [0] * 4 or fading() != [0] * 4, (muted(), fading()))
base0, samp0 = left_pair()
check("... and SAMPLR keeps time for the looper's tracks (its budget is smaller by what they take)", samp0 < base0 - 100_000, (base0, samp0))
for t in range(4):
    if tr(t, MUTED):
        ev(t, MUTE_DOWN)
        ev(t, MUTE_UP)
run_at(0.10, 0.70, 500)
e.call("looper_page_set_leader", 1)                    # the SAMPLR page was opened last: the looper gives way before it
base1, samp1 = left_pair()
check("leader SAMPLR: it does not keep time for the looper (the budgets are equal)", samp1 == base1, (base1, samp1))
o = run_at(0.10, 0.95, 400)
check("... and the looper fades a track at 94 % as it always did", muted() != [0] * 4 or fading() != [0] * 4, (muted(), fading()))
for t in range(4):
    if tr(t, MUTED):
        ev(t, MUTE_DOWN)
        ev(t, MUTE_UP)
run_at(0.10, 0.70, 500)
e.call("looper_page_set_leader", 3)
base3, samp3 = left_pair()
check("no page opened yet: the old order (SAMPLR above the looper)", samp3 == base3, (base3, samp3))
e.call("looper_page_set_leader", 2)
for t in range(4):
    if tr(t, MUTED):
        ev(t, MUTE_DOWN)
        ev(t, MUTE_UP)
run_at(0.10, 0.70, 30)
e.call("looper_guard_force", 1)
run_at(0.99, 0.99, 400)
check("forced off (tests): it does nothing", muted() == [0] * 4, muted())
e.call("looper_guard_force", 0)
print("\n%d FAIL" % fails)
sys.exit(1 if fails else 0)
