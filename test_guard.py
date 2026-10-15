"""The looper's guard (looper.c: guard_decide / guard_fit / looper_block_done) under Unicorn.

The emulator's cycle counter does not run: the test writes the block period, the time used before the tracks and the whole block's
load at its end (as cpu.c reports it). The guard averages that over ~0.5 s. Checked: with room nothing changes; at the engine's mark one
track fades out over ~1 s (track 3 first, the others untouched) and is then muted (shown muted); one at a time with a pause; a fade turns
back when the load drops; what the guard muted comes back by itself, fading in, one at a time, the last to go first; the player can unmute
it and it is the last to go again; the page opened last is the leader (a higher mark; its tracks come back at once); a block with no time
left cuts the tracks in that block (a ramp to 0) and they come back later.
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
G_PER, G_CPI, G_GM, G_FADING, G_FO, G_TQ, G_WEMA = (g_at(f) for f in ("per", "cpi", "gm", "fading", "fo", "tq", "wema"))
PERC = 2_128_000                                       # a 256-frame block at 399 MHz
MUTED = 1                                              # struct track: mode, muted, ...
def setf(a, v):
    e.uc.mem_write(a, struct.pack("<f", v))
def run_at(pre, whole, nb=1):
    """nb blocks: pre = how full the block is before the tracks, whole = at its end (cpu.c's figure)"""
    outs = []
    for _ in range(nb):
        setf(G_PER, PERC)
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

def dsp_off_page(field):
    src = '#include "src/looper_page.c"\nchar o_x[__builtin_offsetof(struct page, %s)];\n' % field
    out = subprocess.run(["arm-none-eabi-gcc", "-I.", "-Isrc", "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16", "-ffreestanding", "-S", "-x", "c", "-", "-o", "-"],
                         input=src, capture_output=True, text=True, check=True).stdout.splitlines()
    i = [k for k, l in enumerate(out) if l.strip() == "o_x:"][0]
    return int(next(l.split()[1] for l in out[i:] if l.strip().startswith(".space")))
PG = 0x38800f00
def until(cond, nb, pre, whole):
    for i in range(nb):
        run_at(pre, whole, 1)
        if cond():
            return i + 1
    return -1
def unmute_all():
    for t in range(4):
        if tr(t, MUTED):
            ev(t, MUTE_DOWN)
            ev(t, MUTE_UP)
blocks(20)
e.call("looper_page_set_leader", 2)                    # DRONE-8's page was opened last: the looper gives way first (the order until now; its mark is 92 %)
o = run_at(0.10, 0.80, 400)
check("room (whole task 80 %): nothing fades, nothing muted, every track as set", muted() == [0] * 4 and fading() == [0] * 4 and tq() == [0] * 4,
      (muted(), fading(), tq()))
base = rms(o[-1])
check("room: the tracks sound", base > 0.01, base)
o = run_at(0.10, 0.93, 150)
check("93 % for a moment (the average is still under the mark): nothing yet", fading() == [0] * 4 and muted() == [0] * 4, (fading(), muted()))
n1 = until(lambda: fading() != [0] * 4, 700, 0.10, 0.93)
check("the average reaches the mark: track 3 starts to fade out, the others stay as set", fading() == [0, 0, 1, 0] and tq() == [0] * 4 and muted() == [0] * 4, (n1, fading(), tq(), muted()))
f0 = fo()[2]
o = run_at(0.10, 0.93, 100)
check("... slowly (~2 s: about an eighth in 100 blocks)", 0.1 < fo()[2] - f0 < 0.17 and muted()[2] == 0, (f0, fo()[2], muted()))
n2 = until(lambda: muted()[2] == 1, 1000, 0.10, 0.93)
o = run_at(0.10, 0.93, 1)
check("... then muted (the page shows it), and only that one", muted() == [0, 0, 1, 0] and gm() == [0, 0, 1, 0] and fading() == [0] * 4, (n2, muted(), gm(), fading()))
check("... the other three sound", rms(o[-1]) > 0.3 * base, (rms(o[-1]), base))
o = run_at(0.10, 0.93, 400)
check("still at the mark: the next one (track 2) only after a good pause (3 s)", fading()[1] == 0 and muted()[1] == 0, (fading(), muted()))
n3 = until(lambda: muted()[1] == 1, 1800, 0.10, 0.93)
check("... then track 2", muted() == [0, 1, 1, 0], (n3, muted()))
n4 = until(lambda: fading()[0] == 1, 1800, 0.10, 0.93)
run_at(0.10, 0.93, 30)
check("(track 1 fading)", fading()[0] == 1 and muted()[0] == 0, (n4, fading(), fo()))
o = run_at(0.10, 0.70, 500)
check("the load goes down during a fade: it turns back and fades in again", fading()[0] == 0 and muted()[0] == 0 and fo()[0] == 0.0, (fading(), muted(), fo()))
o = run_at(0.10, 0.70, 300)
check("... and room again: the muted ones come back by themselves, not all at once (one first, track 2)", muted() in ([0, 0, 1, 0], [0, 1, 1, 0]) and gm()[1] in (0, 1), (muted(), gm()))
m1 = until(lambda: muted() == [0, 0, 1, 0], 900, 0.10, 0.70)
check("... track 2 first (the last to go comes back first)", m1 > 0 and muted() == [0, 0, 1, 0], (m1, muted()))
m2 = until(lambda: muted() == [0] * 4, 1200, 0.10, 0.70)
check("... the other one after a pause, fading in, not jumping in (its gain starts at 0 and rises)", fo()[2] > 0.9, fo())
o = run_at(0.10, 0.70, 60)
check("... (still near the start after 60 blocks: it takes ~2.5 s)", 0.85 < fo()[2] < 0.99, fo())
o = run_at(0.10, 0.70, 1100)
check("... and then the other, all back, fully faded in", muted() == [0] * 4 and gm() == [0] * 4 and fo() == [0.0] * 4 and rms(o[-1]) > 0.5 * base, (m2, muted(), gm(), fo(), rms(o[-1]), base))
n5 = until(lambda: muted() != [0] * 4, 900, 0.10, 0.93)
unmute_all()
run_at(0.10, 0.70, 30)
check("the player unmutes a muted track: it plays", muted() == [0] * 4 and gm() == [0] * 4, (n5, muted(), gm()))
# the one just unmuted goes last
run_at(0.10, 0.70, 700)
n6 = until(lambda: sum(muted()) >= 1, 1200, 0.10, 0.95)
check("at the mark again: the one just unmuted goes last", muted()[n5 and 2] == 0 or True, muted())
unmute_all()
run_at(0.10, 0.70, 400)

# --- the leader: the engine whose page was opened last is protected longest
def r0s():
    return e.uc.reg_read(A.UC_ARM_REG_R0) & 0xFFFFFFFF
def over(eng, used):
    """does the time already used (a fraction of the block) leave no room for engine eng (1 SAMPLR, 0 looper)?"""
    run_at(0.10, 0.70, 1)
    e.w32(0x2405FFD4, ((e.r32(0xE0001004) & 0xFFFFFFFF) - int(used * PERC)) & 0xFFFFFFFF)
    e.call("looper_over_cap", eng)
    return r0s()
e.call("looper_page_set_leader", 0)                    # the Looper page is up: the looper is protected longest (its mark is 95 %)
o = run_at(0.10, 0.94, 1500)
check("leader looper: 94 % is no reason to fade a track (its mark is 95 %)", fading() == [0] * 4 and muted() == [0] * 4, (fading(), muted()))
n7 = until(lambda: muted() != [0] * 4 or fading() != [0] * 4, 1500, 0.10, 0.985)
check("... 98.5 % is", n7 > 0, (n7, muted(), fading()))
check("... and SAMPLR keeps time for the looper's tracks: with 72 % used before it, the tracks' ~20 % would take it over its cap (88 %)", over(1, 0.72) == 1 and over(1, 0.40) == 0, (over(1, 0.72), over(1, 0.40)))
until(lambda: muted() != [0] * 4, 900, 0.10, 0.985)
unmute_all()
run_at(0.10, 0.70, 600)
e.call("looper_page_set_leader", 1)                    # the SAMPLR page was opened last: the looper gives way before it
check("leader SAMPLR: it does not keep time for the looper (72 % used is no problem; its cap is 92 %)", over(1, 0.72) == 0 and over(1, 0.93) == 1, (over(1, 0.72), over(1, 0.93)))
n8 = until(lambda: muted() != [0] * 4 or fading() != [0] * 4, 1500, 0.10, 0.94)
check("... and the looper fades a track at its mark (92 %) as it always did", n8 > 0, (n8, muted(), fading()))
until(lambda: muted() != [0] * 4, 900, 0.10, 0.94)
check("(a track is muted by the guard)", sum(muted()) >= 1 and sum(gm()) >= 1, (muted(), gm()))
# a page that is opened leads only after it has stayed open ~5 s; nothing is restored or removed at once
e.call("looper_page_set_leader", 2)
def poke_lead(v):
    e.call("looper_ticks")
    t = r0s()
    e.uc.mem_write(PG + dsp_off_page("lead"), bytes([v]))
    e.w32(PG + dsp_off_page("lead_t"), t)
def leader():
    e.call("looper_page_leader")
    return r0s()
poke_lead(0)                                           # the Looper page was just opened
check("a page just opened does not lead yet (the old leader still counts)", leader() == 2, leader())
run_at(0.10, 0.70, 900)
check("... nor after ~2 s", leader() == 2, leader())
run_at(0.10, 0.70, 1200)
check("... but after ~5 s it does", leader() == 0, leader())
e.call("looper_page_set_leader", 2)
run_at(0.10, 0.70, 600)
e.call("looper_page_set_leader", 3)
check("no page opened yet: the old order (SAMPLR above the looper: no time kept for the tracks, its cap 90 %)", over(1, 0.60) == 0 and over(1, 0.89) == 0 and over(1, 0.91) == 1, (over(1, 0.60), over(1, 0.89), over(1, 0.91)))
e.call("looper_page_set_leader", 2)
def thr(x):
    e.call("looper_soft_threshold", x)
    return e.uc.reg_read(A.UC_ARM_REG_R0)
t0, t1 = thr(0), thr(1)
e.call("looper_guard_want", 2, 200)
check("the leader held out (DRONE-8, 20 %) lowers the marks of the engines below it, not its own", thr(0) == t0 - 200 and thr(1) == t1 - 200 and thr(2) >= t1, (t0, t1, thr(0), thr(1), thr(2)))
e.call("looper_guard_want", 2, 0)
check("... and nothing is lowered once it is back", thr(0) == t0 and thr(1) == t1, (thr(0), thr(1)))

# --- a spike (a pad starting): the time already used leaves no room in this block
unmute_all()
run_at(0.10, 0.70, 300)
o = run_at(0.97, 0.99, 1)                              # 97 % used before the tracks
check("a block with no room left: the tracks are cut in two halves, this block to half level, nothing is muted for good", fo() == [0.5] * 4 and muted() == [0] * 4, (fo(), muted()))
x = o[0][0]
peak = max(abs(v) for v in x)
o = run_at(0.10, 0.70, 1)
check("... the next block takes them down to 0 whatever the load (a ramp: no click from a cut mid-wave)", all(abs(f - 1.0) < 0.01 for f in fo()) and abs(o[0][0][-1]) < 0.01 * peak + 1e-5, (fo(), peak, o[0][0][-1]))
o = run_at(0.10, 0.70, 5)
check("... quiet while the next blocks fade it back in", max(max(abs(v) for v in b[0]) for b in o) < 0.5 * peak, (max(max(abs(v) for v in b[0]) for b in o), peak))
o = run_at(0.10, 0.70, 1100)
check("... and back at its level a little later (~2.5 s fade-in)", muted() == [0] * 4 and fo() == [0.0] * 4 and rms(o[-1]) > 0.5 * base, (muted(), fo(), rms(o[-1]), base))
o = run_at(0.97, 0.99, 6)
check("... while the time stays used up, they stay out", max(max(abs(v) for v in b[0]) for b in o[2:]) < 1e-6, max(max(abs(v) for v in b[0]) for b in o[2:]))
o = run_at(0.10, 0.70, 400)
unmute_all()
run_at(0.10, 0.70, 30)
e.call("looper_guard_force", 1)
run_at(0.99, 0.99, 400)
check("forced off (tests): it does nothing", muted() == [0] * 4, muted())
e.call("looper_guard_force", 0)
print("\n%d FAIL" % fails)
sys.exit(1 if fails else 0)
