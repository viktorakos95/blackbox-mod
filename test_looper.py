"""Live looper engine under Unicorn (src/looper.c), plus the Looper mode controls in src/solo.c.

Stubbed: the sample pool init (a fake pool is laid out instead), the input stage's tail call (records what it was
handed), the GUI hit test and fill / outline. The Out 1 bus is two fake buffers handed to looper_bus, as comp_process
does (test_comp.py covers that call).
NOT checked: the real pool's behaviour around claimed blocks, how the cells look on the
screen, CPU on the real chip (hardware).
"""
import struct
import sys

import unicorn.arm_const as A

from emu import STACK, Emu

e = Emu("out/solo+slice+duck+chord+cond+filter+cpu+od+comp+seqfix+fx2+munchi+looper+cave/BLACKBOX.bin", "out/cave.elf")
e.uc.mem_map(0x38800000, 0x1000)                     # backup SRAM
e.uc.mem_map(0x58024000, 0x1000)                     # RCC / PWR
e.uc.mem_map(0xC0000000, 0x1000000)                  # SDRAM: the pool's buffers
e.uc.mem_map(0x30000000, 0x40000)                    # fake engine + buffers
ENGINE = 0x30000000
INL, INR, PTRS = 0x30010000, 0x30011000, 0x30012000
OUT = [0x30020000 + 0x1000 * i for i in range(6)]
N = 256
STATE = 0x38800C00
ENTRIES, FIRST, PER = 615, 615 - 4 * 59, 59

fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


def lay_pool():
    """615 entries of 0x1c bytes at the engine; the looper's 236 get real 32 KB buffers, the rest dummies."""
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


def boot():
    lay_pool()
    e.uc.mem_write(0xC0000000, b"\x11" * 0x100)      # garbage: the boot must wipe the tracks
    log.clear()
    e.call("looper_boot", ENGINE, count=100_000_000)


def block(l, r=None, base=0.25):
    r = r if r is not None else l
    e.uc.mem_write(INL, struct.pack(f"<{N}f", *l))
    e.uc.mem_write(INR, struct.pack(f"<{N}f", *r))
    e.w32(PTRS, INL)
    e.w32(PTRS + 4, INR)
    log.clear()
    e.call("looper_in", 0x24005555, PTRS, N, count=20_000_000)
    tail = [x for x in log if x[0] == "tail"]
    for o in OUT:
        e.uc.mem_write(o, struct.pack(f"<{N}f", *([base] * N)))
    e.call("looper_bus", OUT[0], OUT[3], N, count=20_000_000)
    outs = [[v - base for v in struct.unpack(f"<{N}f", e.uc.mem_read(o, 4 * N))] for o in OUT]
    return tail, outs


def cmd(t, c):
    e.call("looper_command", t, c)


REC, MUTE, CLEAR = 1, 2, 3
EMPTY, RECM, PLAY, DUB, CLEARING = range(5)


def mode(t):
    return e.r8(STATE + 0x2C + 16 * t)


def st(off):
    return e.r32(STATE + off)


LEN, POS, RECN, OK = 12, 16, 20, 8

boot()
check("boot: the stock pool init still runs, with the engine", ("pool", ENGINE) in log, log)
check("boot: the last 236 pool blocks claimed (state 3, owner LOOP), the others untouched",
      all(e.r8(ENGINE + 0x1C * i + 0x1F) == 3 and e.r32(ENGINE + 0x1C * i + 0x18) == 0x4C4F4F50 for i in range(FIRST, ENTRIES))
      and all(e.r8(ENGINE + 0x1C * i + 0x1F) == 0 for i in range(FIRST)))
check("boot: track memory wiped", bytes(e.uc.mem_read(0xC0000000, 0x100)) == bytes(0x100))
check("boot: ready, every track empty, no loop", st(OK) == 1 and all(mode(t) == EMPTY for t in range(4)) and st(LEN) == 0)

tail, outs = block([0.5] * N)
check("input stage tail call is made with the same arguments", tail == [("tail", 0x24005555, PTRS, N)], tail)
check("nothing recorded or played while idle", all(v == 0 for o in outs for v in o))


def ramp(k0, n=N):
    return [((k0 + i) % 500 + 1) / 1000.0 for i in range(n)]


# --- first take on track 0 sets the length
cmd(0, REC)
k = 0
for _ in range(40):                                  # 40 blocks = 10240 frames
    _, outs = block(ramp(k), [-v for v in ramp(k)])
    k += N
check("first take: track 0 recording, nothing played meanwhile", mode(0) == RECM and st(RECN) == 40 * N and all(v == 0 for o in outs for v in o))
cmd(0, REC)
_, outs = block([0.0] * N)
check("REC again closes the loop: length = what was recorded, playing", st(LEN) == 40 * N and mode(0) == PLAY, (st(LEN), mode(0)))
# first block after closing: gain ramps up from 0; second block is full level from frame N
_, outs = block([0.0] * N)
exp = ramp(N)
check("loop plays back on the Out 1 bus (left = +, right = -)", all(abs(outs[0][i] - exp[i]) < 2e-4 for i in range(N)) and all(abs(outs[3][i] + exp[i]) < 2e-4 for i in range(N)),
      (outs[0][:3], exp[:3]))
check("seam: the first frames of the take are faded in", e.r16(0xC0000000) == 0)

# the real mix point: the thunk at the "compressor on?" test finds Out 1 like the compressor stage does
FP, OBJ, VT, BUFSET, R5 = 0x30030000, 0x30030000 + 0xFC40, 0x30038000, 0x30038100, 0x30038200
e.w32(OBJ, VT)
e.w32(VT + 0x54, 0x08046A15)
e.uc.mem_write(OBJ + 0x1E, struct.pack("<H", 3))
e.uc.mem_write(R5 + 0xD60, b"\x00")
seen = []
e.stub(0x0805F37C, lambda: seen.append(("bus", e.arg(0), e.arg(1))), value=0x30038300)
e.stub(0x0804D8E8, value=N)
def stereo():
    e.w32(e.arg(1), OUT[1])
    e.w32(e.arg(2), OUT[4])
e.stub(0x0804D9C0, stereo)
for o in (OUT[1], OUT[4]):
    e.uc.mem_write(o, bytes(4 * N))
e.uc.reg_write(A.UC_ARM_REG_R11, FP)
e.uc.reg_write(A.UC_ARM_REG_R5, R5)
e.w32(STACK, BUFSET)
p = st(POS)
e.uc.reg_write(A.UC_ARM_REG_R5, R5)
e.uc.reg_write(A.UC_ARM_REG_R11, FP)
e.call("looper_stage_thunk", 0x1111, 0x2222, 0x3333)
l1 = struct.unpack(f"<{N}f", e.uc.mem_read(OUT[1], 4 * N))
check("thunk: asks for the compressor object's output bus (index 3) of the caller's buffer set", seen == [("bus", BUFSET, 3)], seen)
check("thunk: the loop lands in that bus", all(abs(l1[i] - ramp(p)[i]) < 2e-4 for i in range(N)), (l1[:2], ramp(p)[:2]))
check("thunk: returns the compressor flag in r3 and keeps r0-r2", e.uc.reg_read(A.UC_ARM_REG_R3) == 0 and
      (e.uc.reg_read(A.UC_ARM_REG_R0), e.uc.reg_read(A.UC_ARM_REG_R1), e.uc.reg_read(A.UC_ARM_REG_R2)) == (0x1111, 0x2222, 0x3333))
e.uc.mem_write(R5 + 0xD60, b"\x01")
e.uc.reg_write(A.UC_ARM_REG_R5, R5)
e.uc.reg_write(A.UC_ARM_REG_R11, FP)
e.call("looper_stage_thunk")
check("thunk: compressor on reads back as 1", e.uc.reg_read(A.UC_ARM_REG_R3) == 1)

# wrap: run to the end of the loop and check continuity
for _ in range((st(LEN) - st(POS)) // N):
    block([0.0] * N)
check("playhead wraps at the loop length", st(POS) == 0, st(POS))

# --- overdub on track 1 (empty) while track 0 plays
cmd(1, REC)
_, outs = block([0.1] * N)
check("REC on an empty track with a loop: overdub", mode(1) == DUB)
check("while overdubbing, the new input is not played back again on top of the live input",
      all(abs(outs[0][i] - ramp(0)[i]) < 2e-3 for i in range(96, N)), outs[0][96:99])
for _ in range(39):
    block([0.1] * N)
cmd(1, REC)
_, outs = block([0.0] * N)
check("REC again: track 1 plays", mode(1) == PLAY)
p = st(POS)
_, outs = block([0.0] * N)
check("both tracks now play: track 0 + the 0.1 overdub", all(abs(outs[0][i] - (ramp(p)[i] + 0.1)) < 2e-3 for i in range(N)), (outs[0][:3], ramp(p)[:3]))

# --- mute fades, level scales
cmd(1, MUTE)
p = st(POS)
_, outs = block([0.0] * N)
check("MUTE: track 1 fades out over one block", abs(outs[0][-1] - ramp(p)[-1]) < 2e-3 and outs[0][0] > ramp(p)[0] + 0.09, (outs[0][0], outs[0][-1]))
p = st(POS)
_, outs = block([0.0] * N)
check("...and stays out", all(abs(outs[0][i] - ramp(p)[i]) < 2e-4 for i in range(N)))
def level(t, v):
    e.uc.reg_write(A.UC_ARM_REG_S0, struct.unpack("<I", struct.pack("<f", v))[0])
    e.call("looper_set_level", t)


cmd(1, MUTE)
block([0.0] * N)
level(0, 0.5)
block([0.0] * N)
p = st(POS)
_, outs = block([0.0] * N)
check("level 0.5 on track 0 halves it", all(abs(outs[0][i] - (0.5 * ramp(p)[i] + 0.1)) < 2e-3 for i in range(N)), (outs[0][:2], ramp(p)[:2]))
level(0, 3.0)
check("level is clamped to 1", struct.unpack("<f", e.uc.mem_read(STATE + 0x2C + 8, 4))[0] == 1.0)

# --- clear
cmd(0, CLEAR)
block([0.0] * N)
check("CLEAR: track 0 clearing", mode(0) == CLEARING)
for _ in range(60):
    block([0.0] * N)
check("after about 0.3 s: track 0 empty and wiped", mode(0) == EMPTY and bytes(e.uc.mem_read(0xC0000000, 0x400)) == bytes(0x400))
check("loop length kept while track 1 still plays", st(LEN) == 40 * N)
cmd(1, CLEAR)
for _ in range(62):
    block([0.0] * N)
check("every track empty: loop length free again", st(LEN) == 0 and mode(1) == EMPTY)

# --- 20 s limit
cmd(2, REC)
for _ in range(20 * 48000 // N + 2):
    block([0.01] * N)
check("first take stops itself at 20 s and plays", st(LEN) == 20 * 48000 and mode(2) == PLAY, (st(LEN), mode(2)))

# --- safety: a block taken back by the firmware stops the looper
e.uc.mem_write(ENGINE + 0x1C * (FIRST + 3) + 0x1F, b"\x01")
for _ in range(240):
    tail, outs = block([0.3] * N)
check("a claimed block changing hands: looper off, output untouched", st(OK) == 0 and all(v == 0 for o in outs for v in o))
check("...and the input path still runs", tail == [("tail", 0x24005555, PTRS, N)])

# --- boot with the pool already in use: looper stays off
lay_pool()
e.uc.mem_write(ENGINE + 0x1C * (ENTRIES - 1) + 0x1F, b"\x03")
e.call("looper_boot", ENGINE, count=100_000_000)
check("pool blocks not free at boot: nothing claimed, looper off", st(OK) == 0 and e.r8(ENGINE + 0x1C * FIRST + 0x1F) == 0)
e.call("looper_ready")
check("looper_ready reports it (Looper mode is then not offered)", e.uc.reg_read(A.UC_ARM_REG_R0) == 0)

# --- Looper mode on the Mixer screen (src/solo.c)
boot()
SOLO, VIEW, APP, PTA = 0x2405FF60, 0x24040000, 0x24030000, 0x24038000
e.uc.mem_write(VIEW, bytes(0x2000))
e.uc.mem_write(SOLO, struct.pack("<IBBBBIHH", 0x534F4C4F, 1, 0, 0, 0, VIEW, 0, 0))   # in Solo mode
for i in range(16):
    c = VIEW + 0x3AC + i * 0x1A0
    e.uc.mem_write(c + 0x38, struct.pack("<H", (i // 4) << 4 | (i % 4)))
    e.uc.mem_write(c + 4, struct.pack("<4i", 100 * (i % 4), 60 * (i // 4), 90, 50))
e.uc.mem_write(VIEW + 0x1E40, b"\x01")
e.uc.mem_write(APP + 0x8CA4, b"\x2f")
screens = []
e.stub(0x0809EAEC, lambda: screens.append(e.arg(1)))
e.stub(0x080B5E44)
e.call("solo_mix_pressed", APP, 0, 0, 0)
e.call("solo_set_mode", VIEW, 1)
check("MIX in Solo mode goes to Looper mode (mute screen + looper flag)", screens == [0x2F] and e.r8(SOLO + 6) == 1 and e.r8(SOLO + 4) == 0, (screens, e.r8(SOLO + 4), e.r8(SOLO + 6)))

stock_touch = []
e.stub(0x080B5F44, lambda: stock_touch.append(1))
e.stub(0x080B5EBC, lambda: stock_touch.append(2))
hit = {"cell": 0}
def fake_hit():
    e.w32(e.arg(2), hit["cell"])
    e.ret(1)
e.stubs[0x080B5E78] = fake_hit


def tap(row, col, y=None):
    hit["cell"] = VIEW + 0x3AC + (row * 4 + col) * 0x1A0
    e.uc.mem_write(PTA, struct.pack("<2i", 100 * col + 10, 60 * row + 10 if y is None else y))
    e.call("solo_touch_down", VIEW, PTA, 0)


tap(0, 2)
for _ in range(4):
    block([0.2] * N)
check("tap row 0 column 3: track 3 records", mode(2) == RECM)
tap(0, 2)
block([0.0] * N)
check("tap again: track 3 plays", mode(2) == PLAY)
tap(1, 2)
block([0.0] * N)
check("tap row 1: track 3 muted", e.r8(STATE + 0x2C + 16 * 2 + 1) == 1)
tap(3, 2, y=60 * 3 + 40)
e.uc.mem_write(PTA, struct.pack("<2i", 210, 60 * 3 + 40 - 50))        # drag up by half the 100 px span
e.call("solo_touch_move", VIEW, PTA, 0)
lvl = struct.unpack("<f", e.uc.mem_read(STATE + 0x2C + 16 * 2 + 8, 4))[0]
check("row 3 is a fader: from 1.0, dragging up stays at 1.0", lvl == 1.0, lvl)
e.uc.mem_write(PTA, struct.pack("<2i", 210, 60 * 3 + 40 + 50))        # back down by half the span
e.call("solo_touch_move", VIEW, PTA, 0)
lvl = struct.unpack("<f", e.uc.mem_read(STATE + 0x2C + 16 * 2 + 8, 4))[0]
check("dragging down half the span: level 0.5", abs(lvl - 0.5) < 1e-6, lvl)
check("touches in Looper mode never reach the stock mute toggling", stock_touch == [], stock_touch)

fills = []
e.stub(0x0808EA22, lambda: fills.append((struct.unpack("<4i", e.uc.mem_read(e.arg(0), 16)), e.arg(1))))
e.stub(0x0808E994)
stock = []
e.stub(0x080A43B8, lambda: stock.append(e.arg(0)))
CTX = 0x24039000
e.uc.mem_write(CTX, b"\x01\x00\x00\x00" + struct.pack("<I", 0x24039100))
def draw(row, col):
    fills.clear()
    stock.clear()
    e.call("looper_cell_draw", VIEW + 0x3AC + (row * 4 + col) * 0x1A0, CTX)
    return [c for _, c in fills]
block([0.0] * N)
check("row 0 of a playing track: green, with the cyan playhead line", draw(0, 2)[:2] == [0x0B, 0x1B], draw(0, 2))
check("row 1 of a muted track: red", draw(1, 2) == [0x0C])
check("row 3: dark, then the level bar (teal while muted)", draw(3, 2) == [0x19, 0x1A])
check("empty track: dark", draw(0, 0) == [0x19])
e.uc.mem_write(SOLO + 6, b"\x00")
draw(0, 2)
check("outside Looper mode the stock draw runs", stock == [VIEW + 0x3AC + 2 * 0x1A0] and not fills)

print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
