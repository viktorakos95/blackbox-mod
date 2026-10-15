"""Looper cost per block under Unicorn: instructions and memory accesses by function and region (SDRAM = the pool buffers, on the
unit; internal = stack and firmware tables). Usage: PYTHONPATH=. python3 tools/prof_mem.py plain|filt|filt+drive|filt+drive+send|speed|ptch|...
(4 tracks playing; + joins settings). Setup copied from tools/prof_looper.py.

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



import bisect, collections, sys, subprocess
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
# Instructions and memory accesses per block, by function and by region. On the unit the pool buffers (the tracks and the
# effect memory with the looper's working state) are in SDRAM; the stack and the firmware's tables are in internal RAM.
syms = sorted((int(l.split()[0], 16), l.split()[2]) for l in subprocess.run(["arm-none-eabi-nm", "out/cave.elf"], capture_output=True, text=True).stdout.splitlines() if len(l.split()) == 3 and l.split()[1] in "tTwW")
addrs = [a for a, _ in syms]
cnt = collections.Counter(); tot = [0]; mem = collections.Counter(); fmem = collections.Counter(); cur = ["?"]
def fn(a):
    i = bisect.bisect_right(addrs, a) - 1
    return syms[i][1] if i >= 0 else "?"
def hk(uc, a, size, _):
    tot[0] += 1
    cur[0] = fn(a)
    cnt[cur[0]] += 1
def region(a):
    if 0xC0000000 <= a < 0xD0000000: return "sdram"
    if 0x30030000 <= a < 0x30040000: return "sdram"      # the fake pool's other buffers (effect memory): SDRAM on the unit
    if 0x38800000 <= a < 0x38801000: return "bkp"
    return "internal"
def hm(uc, access, a, size, v, _):
    r = region(a)
    mem[r] += 1
    if r == "sdram": fmem[cur[0]] += 1
boot()
scen = sys.argv[1] if len(sys.argv) > 1 else "plain"
opt(O_PITCH, 1)
hold(0, 60, tone)
for tt in (1, 2, 3):
    hold(tt, 60)
for t in range(4):
    if "filt" in scen: param(t, P_FILT, -0.5); param(t, P_RES, 0.5)
    if "drive" in scen: param(t, P_DRIVE, 0.5)
    if "send" in scen: param(t, P_SD, 0.3); param(t, P_SR, 0.3)
    if "speed" in scen: param(t, P_SPEED, 0.3)
    if "ptch" in scen: param(t, P_PTCH, 0.2)
    if "crunch" in scen: param(t, P_CRUNCH, 0.5)
blocks(30)
print("modes", [mode(t) for t in range(4)], "len", [tu(t, T_LEN) for t in range(4)])
h = e.uc.hook_add(UC_HOOK_CODE, hk)
h2 = e.uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, hm)
NB = 6
for _ in range(NB):
    block([0.0] * N)
e.uc.hook_del(h); e.uc.hook_del(h2)
for k, v in cnt.most_common(8): print(f"   instr {k:24s} {v // NB:8d}")
print(scen, "instructions/block:", tot[0] // NB, " mem/block:", {k: v // NB for k, v in mem.items()})
for k, v in fmem.most_common(8):
    print(f"   sdram {k:24s} {v // NB:8d}")
