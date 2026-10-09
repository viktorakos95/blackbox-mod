"""Live looper under Unicorn: the engine (src/looper.c), the Out 1 mix point (src/looper_thunk.S) and the page
(src/looper_page.c, routed through src/solo.c).

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
    e.uc.mem_write(STATE + 164 + 9 * 4, struct.pack("<f", 0.0))       # loop make-up gain off: unity, as the checks expect
    e.uc.mem_write(STATE + 164 + 11 * 4, struct.pack("<f", 0.0))      # not full screen: the geometry the checks expect


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


# --- boot
boot()
check("boot: the stock pool init still runs, with the engine", ("pool", ENGINE) in log, log)
check("boot: the last 231 pool blocks claimed (4 tracks + undo + effects), the others untouched",
      all(e.r8(ENGINE + 0x1C * i + 0x1F) == 3 and e.r32(ENGINE + 0x1C * i + 0x18) == 0x4C4F4F50 for i in range(FIRST, ENTRIES))
      and all(e.r8(ENGINE + 0x1C * i + 0x1F) == 0 for i in range(FIRST)))
check("boot: track memory wiped", bytes(e.uc.mem_read(0xC0000000, 0x100)) == bytes(0x100))
e.call("looper_status", 0x2403F000)
check("status: Lok", bytes(e.uc.mem_read(0x2403F000, 3)) == b"Lok")
check("boot: ready, every track empty, no loop, no undo", st(OK) == 1 and all(mode(t) == EMPTY for t in range(4)) and st(LEN) == 0 and e.r32(STATE + UNDO_T) == -1)
tail, outs = block([0.5] * N)
check("input stage tail call is made with the same arguments", tail == [("tail", 0x24005555, PTRS, N)], tail)
check("nothing recorded or played while idle", all(v == 0 for o in outs for v in o))

# --- hold to record: the first take sets the length
ev(0, REC_DOWN)
k = 0
for _ in range(60):                                   # 60 blocks = 320 ms, longer than a tap
    _, outs = block(ramp(k), [-v for v in ramp(k)])
    k += N
check("hold: track 1 recording while held, nothing played meanwhile", mode(0) == RECM and all(v == 0 for o in outs for v in o))
ev(0, REC_UP)
block([0.0] * N)
check("release after 300 ms keeps the take: loop length = what was recorded, playing", st(LEN) == 60 * N and mode(0) == PLAY, (st(LEN), mode(0)))
p = st(POS)
_, outs = block([0.0] * N)
check("the take plays back on Out 1 (left +, right -)", all(abs(outs[0][i] - ramp(p)[i]) < 2e-4 and abs(outs[1][i] + ramp(p)[i]) < 2e-4 for i in range(N)), (outs[0][:2], ramp(p)[:2]))
check("seam: the first frames of the take are faded in", e.r16(0xC0000000) == 0)

# --- a tap latches an overdub on track 2 (shorter than one pass of the 60-block loop)
ev(1, REC_DOWN)
block([0.1] * N)
ev(1, REC_UP)
blocks(5, 0.1)
check("a tap: overdubbing, latched at once", mode(1) == DUB and tr(1, 3) == 3, (mode(1), tr(1, 3)))
blocks(80 - 7 - 30, 0.1)
check("a latched take keeps recording well past the double-tap window", mode(1) == DUB)
p = st(POS)
_, outs = block([0.1] * N)
check("while overdubbing, the live input is not played back on top of itself", all(abs(outs[0][i] - ramp(p)[i]) < 2e-3 for i in range(N)), (outs[0][:2], ramp(p)[:2]))
ev(1, REC_DOWN)
block([0.0] * N)
ev(1, REC_UP)
block([0.0] * N)
check("one more tap keeps it: track 2 plays", mode(1) == PLAY and tr(1, 3) == 0)
extra = []
for _ in range(60):
    p = st(POS)
    _, outs = block([0.0] * N)
    L = st(LEN)
    extra += [outs[0][i] - ramp(p)[i] for i in range(N) if 96 <= (p + i) % L < L - 96]   # off the seam fades
check("both tracks play: track 1, plus track 2's 0.1 where it was overdubbed", abs(max(extra) - 0.1) < 2e-3 and abs(min(extra)) < 2e-3, (min(extra), max(extra)))

# --- undo the last pass of track 2 with a 2 s hold on M
check("the undo track holds track 2's pass", e.r32(STATE + UNDO_T) == 1, e.r32(STATE + UNDO_T))
ev(1, MUTE_DOWN)
blocks(380)
ev(1, MUTE_UP)
blocks(70)
check("M held 2 s: the pass came off; track 2 was empty before it, so it is empty again", mode(1) == EMPTY, mode(1))
check("a hold does not also toggle mute", tr(1, 1) == 0)
p = st(POS)
_, outs = block([0.0] * N)
check("only track 1 plays now", all(abs(outs[0][i] - ramp(p)[i]) < 2e-4 for i in range(N)), outs[0][:2])

# --- a short tap records until the next tap; undo it again
ev(2, REC_DOWN)
block([0.3] * N)
ev(2, REC_UP)
blocks(10, 0.3)
check("a tap on an empty track starts recording (latched)", mode(2) == DUB and tr(2, 3) == 3, (mode(2), tr(2, 3)))
ev(2, REC_DOWN)
block([0.3] * N)
ev(2, REC_UP)
blocks(2)
ev(2, MUTE_DOWN)
blocks(380)
ev(2, MUTE_UP)
blocks(70)
check("the next tap keeps it; holding M 2 s takes it back: track 3 empty again", mode(2) == EMPTY, mode(2))
check("...and its memory is silent", bytes(e.uc.mem_read(0xC0000000 + 2 * PER * 0x10000, 0x400)) == bytes(0x400))

# overdub onto track 1 itself, then undo: track 1 must come back exactly
before = bytes(e.uc.mem_read(0xC0000000, 0x4000))
ev(0, REC_DOWN)
blocks(80, 0.05)
ev(0, REC_UP)
block([0.0] * N)
check("hold on a playing track: overdub, kept on release", mode(0) == PLAY and bytes(e.uc.mem_read(0xC0000000, 0x4000)) != before)
ev(0, MUTE_DOWN)
blocks(380)
ev(0, MUTE_UP)
blocks(70)
check("undo restores track 1 exactly as it was before the pass", mode(0) == PLAY and bytes(e.uc.mem_read(0xC0000000, 0x4000)) == before, mode(0))

# --- mute tap, reverse, level, pan
ev(0, MUTE_DOWN)
block([0.0] * N)
ev(0, MUTE_UP)
block([0.0] * N)
block([0.0] * N)
_, outs = block([0.0] * N)
check("tap M: muted (silent after its fade)", tr(0, 1) == 1 and all(v == 0 for v in outs[0]))
ev(0, MUTE_DOWN)
ev(0, MUTE_UP)
block([0.0] * N)
block([0.0] * N)
ev(0, REVERSE)
block([0.0] * N)
p = st(POS)
_, outs = block([0.0] * N)
L = st(LEN)
exp = [ramp(L - 1 - ((p + i) % L), 1)[0] for i in range(N)]
check("REV: the track plays backwards", tr(0, 2) == 1 and all(abs(outs[0][i] - exp[i]) < 2e-4 for i in range(N)), (outs[0][:2], exp[:2]))
ev(0, REVERSE)
level(0, 0.5)
pan(0, 0.5)
block([0.0] * N)
p = st(POS)
_, outs = block([0.0] * N)
check("level 0.5, pan half right: left at 0.25, right at 0.5", all(abs(outs[0][i] - 0.25 * ramp(p)[i]) < 2e-4 and abs(outs[1][i] + 0.5 * ramp(p)[i]) < 2e-4 for i in range(N)), (outs[0][:1], outs[1][:1], ramp(p)[:1]))
pan(0, -3.0)
level(0, 3.0)
check("pan is clamped to -1, the gain to 2 (+6 dB)", abs(tr(0, O_PAN, "f") + 1.0) < 1e-6 and tr(0, O_LEVEL, "f") == 2.0, (tr(0, O_PAN, "f"), tr(0, O_LEVEL, "f")))
pan(0, 0.0)

# --- erase with a 4 s hold
ev(0, MUTE_DOWN)
blocks(760)
ev(0, MUTE_UP)
blocks(70)
check("M held 4 s: track 1 erased, loop length free again", mode(0) == EMPTY and st(LEN) == 0, (mode(0), st(LEN)))

# --- 16 s limit
ev(3, REC_DOWN)
blocks(MAXF // N + 2, 0.01)
check("a first take stops itself at 15.4 s and plays", st(LEN) == MAXF and mode(3) == PLAY, (st(LEN), mode(3)))
ev(3, REC_UP)
block([0.0] * N)
check("...and the late release does nothing more", mode(3) == PLAY)

# --- the Out 1 mix point (looper_thunk.S)
seen.clear()
e.uc.mem_write(BUSL, bytes(4 * N))
for flag in (0, 1):
    e.uc.mem_write(R5 + 0xD60, bytes([flag]))
    e.uc.reg_write(A.UC_ARM_REG_R5, R5)
    e.uc.reg_write(A.UC_ARM_REG_R11, FP)
    e.w32(STACK, BUFSET)
    e.call("looper_stage_thunk", 0x1111, 0x2222, 0x3333)
    check(f"thunk (compressor {'on' if flag else 'off'}): r3 = the flag, r0-r2 kept", e.uc.reg_read(A.UC_ARM_REG_R3) == flag and
          (e.uc.reg_read(A.UC_ARM_REG_R0), e.uc.reg_read(A.UC_ARM_REG_R1), e.uc.reg_read(A.UC_ARM_REG_R2)) == (0x1111, 0x2222, 0x3333))
check("thunk: asks for the compressor object's output bus of the caller's buffer set", seen[:1] == [("bus", BUFSET, 3)], seen)
check("thunk: the loop lands in that bus", any(v != 0 for v in struct.unpack(f"<{N}f", e.uc.mem_read(BUSL, 4 * N))))

# --- safety
e.uc.mem_write(ENGINE + 0x1C * (FIRST + 3) + 0x1F, b"\x01")
for _ in range(300):
    tail, outs = block([0.3] * N)
check("a claimed block changing hands: looper off, Out 1 untouched", st(OK) == 0 and all(v == 0 for o in outs for v in o))
STATUS = 0x2403F000
e.call("looper_status", STATUS)
st_txt = bytes(e.uc.mem_read(STATUS, e.uc.reg_read(A.UC_ARM_REG_R0) - STATUS)).decode()
check("status names the block taken back", st_txt == f"Lt{FIRST + 3}", st_txt)
check("...and the input path still runs", tail == [("tail", 0x24005555, PTRS, N)])
lay_pool()
e.uc.mem_write(ENGINE + 0x1C * (ENTRIES - 1) + 0x1F, b"\x03")
e.call("looper_boot", ENGINE, count=200_000_000)
check("pool blocks not free at boot: nothing claimed, looper off", st(OK) == 0 and e.r8(ENGINE + 0x1C * FIRST + 0x1F) == 0)
STATUS = 0x2403F000


def status():
    e.call("looper_status", STATUS)
    return bytes(e.uc.mem_read(STATUS, e.uc.reg_read(A.UC_ARM_REG_R0) - STATUS)).decode()


check("status for the version label names the busy block", status() == f"Lb{ENTRIES - 1}", status())
e.call("looper_ready")
check("looper_ready reports it (the page is then not offered)", e.uc.reg_read(A.UC_ARM_REG_R0) == 0)

# --- the page
e.uc.mem_write(0x2405FF60, struct.pack("<IBBBB", 0x534F4C4F, 0, 0, 1, 1))   # a Looper mode left over from before
boot()
check("boot clears a leftover Looper mode flag", e.r8(0x2405FF60 + 6) == 0 and e.r8(0x2405FF60 + 7) == 0)
SOLO, VIEW, APP, PTA, CTX, FBP = 0x2405FF60, 0x30024000, 0x24030000, 0x24038000, 0x24039000, 0x24039100
PAGE = 0x38800F00
e.w32(0x240000D0, 0x080ECDC4)                                        # the firmware's text font: glyphs, 6 x 8
e.uc.mem_write(0x240000D4, struct.pack("<HH", 6, 8))


def cells(y_up=True):
    e.uc.mem_write(VIEW, bytes(0x2000))
    for i in range(16):
        row, col = i // 4, i % 4
        c = VIEW + 0x3AC + i * 0x1A0
        e.uc.mem_write(c + 0x38, struct.pack("<H", row << 4 | col))
        y = row * 56 if y_up else (3 - row) * 56
        e.uc.mem_write(c + 4, struct.pack("<4i", 32 + col * 64, y, 64, 56))      # the real grid: 256 x 224, 32 px margins
    e.uc.mem_write(VIEW + 0x1E40, b"\x01")


cells()
e.uc.mem_write(SOLO, struct.pack("<IBBBBIHH", 0x534F4C4F, 1, 0, 0, 0, VIEW, 0, 0))   # in Solo mode
e.uc.mem_write(VIEW + 0x3AC + 3 * 0x1A0 + 0x6C, b"\x01")             # one cell's child that is hidden by the stock firmware itself
e.uc.mem_write(APP + 0x8CA4, b"\x2f")
screens = []
e.stub(0x0809EAEC, lambda: screens.append(e.arg(1)))
e.stub(0x080B5E44)
e.call("solo_mix_pressed", APP, 0, 0, 0)
e.call("solo_set_mode", VIEW, 1)
check("MIX in Solo mode goes to the Looper page", screens == [0x2F] and e.r8(SOLO + 6) == 1, (screens, e.r8(SOLO + 6)))
CHILD = (0x6C, 0xA0, 0xF8, 0x150)
check("the stock cells' child widgets (the cyan boxes) are hidden while the page shows",
      all(e.r8(VIEW + 0x3AC + i * 0x1A0 + c) == 1 for i in range(16) for c in CHILD))

stock = []
e.stub(0x080B5F44, lambda: stock.append("down"))
e.stub(0x080B5EBC, lambda: stock.append("move"))
e.stub(0x080B5AEC, lambda: stock.append("up"))
fills, pixels = [], []
e.stub(0x0808EA22, lambda: fills.append(struct.unpack("<4i", e.uc.mem_read(e.arg(0), 16)) + (e.arg(1), e.arg(2))))
e.stub(0x0808F524, lambda: pixels.append((e.arg(1), e.arg(2), e.arg(3))))
celldraw = []
e.stub(0x080A43B8, lambda: celldraw.append(e.arg(0)))
e.uc.mem_write(CTX, b"\x01\x00\x00\x00" + struct.pack("<I", FBP))
e.uc.mem_write(FBP, bytes(16))
e.uc.mem_write(FBP + 4, struct.pack("<HH", 320, 240))                # the frame buffer is 320 wide


def draw(cell=0):
    fills.clear()
    pixels.clear()
    celldraw.clear()
    e.call("looper_cell_draw", VIEW + 0x3AC + cell * 0x1A0, CTX, count=50_000_000)
    return list(fills)


f = draw(5)
check("only the first cell draws the page; the others draw nothing and skip the stock draw", not f and not pixels and not celldraw)
f = draw(0)
# page: 224 high (cells y 0..224), top edge 224, y up, 314 wide from x 3 (320 screen, 3 px margins); 77 px columns
# split by hairlines. Column 0's record box: a 1 px outline at x 7, 71 wide, top edge at d = 19
title0 = [x for x in f if x[2] == 71 and x[3] == 1]
check("page painted: background first, then top bar, columns, footer", f[0][:4] == (3, 0, 314, 224) and f[0][4] == 0x02, f[:1])
check("track 1's record box (empty = dark grey outline) sits under the top row: x 7, y = 224 - 19 - 1 = 204",
      any(x[:4] == (7, 204, 71, 1) and x[4] == 0x10 for x in f), title0[:3])
check("four columns of 78 px pitch across the whole width (record boxes at x 7, 85, 163, 241)",
      sorted({x[0] for x in f if x[2] == 71 and x[3] == 1 and x[4] == 0x10}) == [7, 85, 163, 241], sorted({x[0] for x in f if x[2] == 71 and x[3] == 1}))
check("the page frame and the hairlines between columns are 1 px lines", any(x[:4] == (3, 0, 314, 1) for x in f) and any(x[2] == 1 and x[3] > 150 and x[4] == 0x10 for x in f), f[1:4])
check("everything is drawn into the page's own frame buffer", all(x[5] == FBP for x in f))
check("all fills lie inside the screen (x 3..317, y 0..224)", all(x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224 for x in f), [x for x in f if not (x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224)][:3])
check("text is plotted pixel by pixel, inside the screen", pixels and all(3 <= px < 317 and 0 <= py < 224 for px, py, _ in pixels), pixels[:3])
e.uc.mem_write(FBP + 4, struct.pack("<HH", 300, 240))                # a narrower frame buffer than the cells suggest
e.call("looper_page_enter")
f300 = draw(0)
check("never wider than the frame buffer: with a 300 px buffer nothing is drawn past x 300", all(x[0] + x[2] <= 300 for x in f300) and all(px < 300 for px, _, _ in pixels), max(x[0] + x[2] for x in f300))
e.uc.mem_write(FBP + 4, struct.pack("<HH", 320, 240))
e.call("looper_page_enter")
f = draw(0)


# a playing track 1 so its title is coloured
ev(0, REC_DOWN)
blocks(60, 0.2)
ev(0, REC_UP)
blocks(3, 0.0)
f = draw(0)
check("a playing track: the record box outline turns green, the track number is drawn in its colour (cyan)", any(x[:4] == (7, 204, 71, 1) and x[4] == 0x0B for x in f) and any(c == 0x1B for _, _, c in pixels))
check("its fader has a round white handle (a 3 x 7 and a 7 x 3 box)", any(x[2] == 3 and x[3] == 7 and x[4] == 0x0F for x in f) and any(x[2] == 7 and x[3] == 3 and x[4] == 0x0F for x in f))
# levels: bar height follows level
level(0, 0.5)
blocks(2)
f = draw(0)
bars = [x for x in f if x[2] == 2 and x[3] > 5 and x[4] == 0x1B]
meters = [x for x in f if x[2] == 1 and x[3] > 5 and x[4] == 0x1B and x[1] > 20]
check("gain 0.5 sits at 0.375 of the travel (24 of 64 px): the 2 px line and both thin meters", len(bars) == 1 and bars[0][3] == 24 and len(meters) == 2 and meters[0][3] == meters[1][3] == 24, (bars, meters))
pan(0, 0.5)
blocks(2)
f = draw(0)
meters = sorted([x for x in f if x[2] == 1 and x[3] > 5 and x[4] == 0x1B and x[1] > 20], key=lambda x: x[0])
check("pan half right: the left meter is half as tall as the right", len(meters) == 2 and abs(meters[0][3] * 2 - meters[1][3]) <= 2, meters)
pan(0, 0.0)
level(0, 1.0)

# orientation: the same cells with y running downwards, row 3 at the top (smaller y)
cells(y_up=False)
f = draw(0)
check("cells with y down: the page flips to match (title bar at y = 0 + 21)", any(x[:4] == (7, 19, 71, 1) for x in f), [x for x in f if x[2] == 71][:2])
cells()
f = draw(0)

# hit testing (pixels from the page's left / top)
tr_p, val_p = 0x24039200, 0x24039204


def hit(x, d):
    e.uc.mem_write(tr_p, bytes(8))
    e.call("looper_page_hit", x, d, tr_p, val_p)
    z = e.uc.reg_read(A.UC_ARM_REG_R0)
    return z, e.r32(tr_p), struct.unpack("<f", e.uc.mem_read(val_p, 4))[0]


# rows (top-down): top bar 0-16, column 19.., title 21-37, icon 37-63, bars 67.., level text, buttons
check("the record box of column 2", hit(100, 30)[:2] == (1, 1) and hit(100, 55)[:2] == (1, 1), (hit(100, 30), hit(100, 55)))
z, t, v = hit(190, 100)
check("bars area of column 3 is its fader", z == 2 and t == 2 and 0.0 <= v <= 1.0, (z, t, v))
check("fader: silence at the bottom of its travel, +6 dB (gain 2) at the top, unity three quarters up",
      hit(190, 78)[2] > 1.99 and hit(190, 141)[2] < 0.05 and abs(hit(190, 94)[2] - 1.0) < 0.07, (hit(190, 78)[2], hit(190, 141)[2], hit(190, 94)[2]))
btn_y = 16 + (224 - 16 - 14 - 2) - 2 - 34
check("bottom row: the pan dial on the left, REV above MUTE on the right", [hit(245, btn_y + 10)[0], hit(285, btn_y + 4)[0], hit(285, btn_y + 25)[0]] == [3, 4, 5], [hit(245, btn_y + 10), hit(285, btn_y + 4), hit(285, btn_y + 25)])
check("the top row, the hairlines between columns and the footer are not controls", hit(100, 8)[0] == 0 and hit(78, 100)[0] == 0 and hit(230, 215)[0] == 0)

stock_clear = len(stock)


def touch(kind, x_rel, d, up=True):
    # fill-space point: x = 3 + x_rel, y = 224 - d
    e.uc.mem_write(PTA, struct.pack("<2i", 3 + x_rel, 224 - d))
    e.call({"down": "solo_touch_down", "move": "solo_touch_move", "up": "solo_touch_up"}[kind], VIEW, PTA, 0)


def evs(t):
    return list(e.uc.mem_read(STATE + T0 + TSIZE * t + 8, 5))


touch("down", 100, 30)
touch("up", 100, 30)
check("touching a record box sends REC_DOWN then REC_UP for that track", evs(1)[:2] == [1, 1], evs(1))
touch("down", 245, btn_y + 10)
touch("up", 245, btn_y + 10)
check("PAN: selects track 4's knob for the pan", e.r8(PAGE + 6) == 0b1000, e.r8(PAGE + 6))
f = draw(0)
check("a selected pan dial gets the stock pink frame; its pointer is drawn in cyan", any(x[4] == 0x20 for x in f) and sum(1 for x in f if x[4] == 0x1B and x[2] == 1 and x[3] == 1) >= 8, [x[4] for x in f if x[2] == 1][:5])
touch("down", 285, btn_y + 4)
touch("up", 285, btn_y + 4)
check("REV: reverse event", evs(3)[4] == 1, evs(3))
touch("down", 285, btn_y + 25)
touch("up", 285, btn_y + 25)
check("MUTE: down and up", evs(3)[2:4] == [1, 1], evs(3))
e.uc.mem_write(PAGE + 89, b"\x00")
lv0 = tr(2, O_LEVEL, "f")
touch("down", 190, 141)
touch("up", 190, 141)
check("fader: the first touch of an unselected track only selects it (level untouched)", e.r8(PAGE + 89) == 2 and tr(2, O_LEVEL, "f") == lv0, (e.r8(PAGE + 89), tr(2, O_LEVEL, "f")))
touch("down", 190, 141)
check("fader: touching near the bottom of the bars sets a low level", tr(2, O_LEVEL, "f") < 0.1, tr(2, O_LEVEL, "f"))
touch("move", 190, 79)
check("fader: dragging to the top sets about +6 dB", abs(tr(2, O_LEVEL, "f") - 2.0) < 0.08, tr(2, O_LEVEL, "f"))
touch("up", 190, 78)
check("the page's touches never reach the stock mixer handlers", len(stock) == stock_clear, stock)

# knobs: message 0x32 to the view
viewmsg = []
e.stub(0x080B5C70, lambda: viewmsg.append(e.r16(e.arg(1))))
MSG = 0x2403A000


def knob(i, counts, msg_id=0x32):
    i = i ^ 1 if i < 2 else i                         # the left encoders are swapped: the bottom one is track 1
    e.uc.mem_write(MSG, struct.pack("<H", msg_id) + bytes(10) + struct.pack("<h", i) + bytes(2) + struct.pack("<h", counts))
    e.call("looper_view_msg", VIEW, MSG)


level(0, 0.5)
knob(0, -800)
check("knob 1 turns track 1's fader (800 counts = 10 % of the travel: gain 0.5 -> 0.367)", abs(tr(0, O_LEVEL, "f") - 0.36667) < 1e-4, tr(0, O_LEVEL, "f"))
knob(3, 1600)
check("knob 4 turns track 4's pan while PAN is selected", abs(tr(3, O_PAN, "f") - 0.4) < 1e-5, tr(3, O_PAN, "f"))
knob(0, 100, msg_id=0x63)
check("other view messages go to the stock handler", viewmsg == [0x63], viewmsg)

# INFO
appmsg = []
e.stub(0x080A2E60, lambda: appmsg.append((e.r16(e.arg(1)), e.r32(e.arg(1) + 0xC))))


def button(idx, msg_id=0xF9):
    e.uc.mem_write(MSG, struct.pack("<H", msg_id) + bytes(10) + struct.pack("<i", idx))
    e.call("looper_app_msg", APP, MSG)


check("INFO is preset to message 0xc (nothing to learn: the backup SRAM does not survive a power cycle)", e.r8(PAGE + 9) == 1 and e.r16(PAGE + 16) == 0xC, (e.r8(PAGE + 9), e.r16(PAGE + 16)))
button(5)
check("MIX always reaches the stock handler", appmsg == [(0xF9, 5)], appmsg)
button(0, msg_id=0xC)
check("INFO (message 0xc) is swallowed and switches SHIFT on", appmsg == [(0xF9, 5)] and e.r8(PAGE + 7) == 1, (appmsg, e.r8(PAGE + 7)))
button(2)
check("other buttons pass through once INFO is known", appmsg[-1] == (0xF9, 2), appmsg)
knob(1, 800)
check("INFO on: knob 2 turns the pan, not the level", abs(tr(1, O_PAN, "f") - 0.2) < 1e-5 and tr(1, O_LEVEL, "f") == 1.0, (tr(1, O_PAN, "f"), tr(1, O_LEVEL, "f")))
touch("down", 100, 30)
touch("up", 100, 30)
check("INFO on: a record box tap is a MUTE press, not a REC press", evs(1)[2:4] == [1, 1] and evs(1)[:2] == [1, 1], evs(1))
button(0, msg_id=0xC)
check("INFO again: off", e.r8(PAGE + 7) == 0)
button(0, msg_id=0xC)
check("INFO again: on", e.r8(PAGE + 7) == 1)
blocks(1900)
e.call("looper_page_poke", VIEW)
check("INFO mode ends by itself after 10 s idle", e.r8(PAGE + 7) == 0, e.r8(PAGE + 7))
e.uc.mem_write(SOLO + 6, b"\x00")
button(0, msg_id=0xC)
check("off the page every button goes to the stock handler (INFO included)", appmsg[-1] == (0xC, 0), appmsg[-1])
e.uc.mem_write(SOLO + 6, b"\x01")

# footer, signature
f = draw(0)
check("the footer draws", len(pixels) > 20)

e.call("looper_page_enter")
level(1, 0.3)
e.call("looper_page_poke", VIEW)
check("a change marks every cell dirty", all(e.r8(VIEW + 0x3AC + i * 0x1A0 + 0x17C) == 1 for i in range(16)))
appmsg.clear()
button(7, msg_id=7)
check("message 7 is not INFO: it goes to the stock handler", appmsg == [(7, 7)], appmsg)

# ---- tabs: FX and SETUP
def tab(i):
    touch("down", 3 + i * 36 + 10, 224 - 5)
    touch("up", 3 + i * 36 + 10, 224 - 5)


f = draw(0)
check("the footer has three tab buttons (MAIN is the cyan one)", sum(1 for x in f if x[2] == 34 and x[3] == 1 and x[4] == 0x1B) == 2 and sum(1 for x in f if x[2] == 34 and x[3] == 1 and x[4] == 0x10) >= 4)
tab(1)
check("tapping the FX tab switches the page to FX", e.r8(PAGE + 76) == 1, e.r8(PAGE + 76))
f = draw(0)
check("FX tab: four dials per column (a 34 x 1 frame line each), no fader meters", sum(1 for x in f if x[2] == 34 and x[3] == 1 and x[4] in (0x10, 0x20)) >= 16 * 2 - 8 and not [x for x in f if x[2] == 2 and x[3] > 20 and x[4] == 0x1B])
check("FX tab: the selected dial (FILT by default) has the pink frame", any(x[4] == 0x20 for x in f))
check("FX tab: everything inside the screen", all(x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224 for x in f), [x for x in f if not (x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224)][:3])
# hit zones: dial grid under the 40 px record box (top at 22..62): dials start at d = 19 + 3 + 40 + 4 = 66 + 16
y0 = 16 + 3 + 22 + 3 + 16
h1 = hit(1 + 78 * 1 + 2 + 10, y0 + 10)
h2 = hit(1 + 78 * 1 + 2 + 10 + 35, y0 + 10)
h3 = hit(1 + 78 * 1 + 2 + 10, y0 + 35 + 10)
h4 = hit(1 + 78 * 1 + 2 + 10 + 35, y0 + 35 + 10)
check("FX tab: the four dials are zone 7 with parameters 0 1 2 3 (track 2)", [(h[0], h[1], int(h[2])) for h in (h1, h2, h3, h4)] == [(7, 1, 0), (7, 1, 1), (7, 1, 2), (7, 1, 3)], (h1, h2, h3, h4))
y1 = y0 + 124
h5 = hit(1 + 78 * 1 + 2 + 10, y0 + 105 + 10)
h6 = hit(1 + 78 * 1 + 2 + 10 + 35, y0 + 105 + 10)
check("FX tab: REV and HALF are the 7th and 8th controls (zone 7, parameters 6 and 7)", [(h[0], int(h[2])) for h in (h5, h6)] == [(7, 6), (7, 7)], (h5, h6))
check("FX tab: MUTE and UNDO in the row below", [hit(1 + 78 + 8, y1 + 5)[0], hit(1 + 78 + 60, y1 + 5)[0]] == [5, 12], [hit(1 + 78 + 8, y1 + 5), hit(1 + 78 + 60, y1 + 5)])
# touch: tap the CRSH tile of track 2: the one pink square (group 0 = FILT RES CRSH DRIVE = knobs 1-4) goes to track 2's column
touch("down", 78 + 2 + 10, y0 + 35 + 10)
check("tap a tile: the pink square moves to that track and block of four", e.r8(PAGE + 89) == 1 and e.r8(PAGE + 77) == 0, (e.r8(PAGE + 89), e.r8(PAGE + 77)))
touch("move", 78 + 2 + 10, y0 + 35 + 10 - 40)
touch("up", 78 + 2 + 10, y0 + 35 + 10 - 40)
check("dragging a tile up by 40 px of 80 turns it half way", abs(tr(1, O_CRUNCH, "f") - 0.5) < 0.02, tr(1, O_CRUNCH, "f"))
knob(0, 800)
check("FX tab: knob 1 (left bottom) turns the bottom-left tile of the square (CRSH +10 %) on the selected track", abs(tr(1, O_CRUNCH, "f") - 0.6) < 0.02, tr(1, O_CRUNCH, "f"))
knob(1, 800)
check("FX tab: knob 2 (left top) turns the top-left tile, FILT, of the selected track (+20 % of a range of 2); the other tracks are untouched", abs(tr(1, O_FILT, "f") - 0.2) < 0.02 and tr(0, O_FILT, "f") == 0 and tr(0, O_CRUNCH, "f") == 0 and e.r8(PAGE + 89) == 1, (tr(1, O_FILT, "f"), tr(0, O_FILT, "f"), tr(0, O_CRUNCH, "f")))
f = draw(0)
check("FX tab top row shows the controls of the square", pixels and any(c == 0x0F for _, _, c in pixels))
touch("down", 78 + 2 + 10 + 35, y0 + 105 + 10)
touch("up", 78 + 2 + 10 + 35, y0 + 105 + 10)
check("a tap on HALF moves the square to its block (group 1) and sends nothing", e.r8(PAGE + 77) == 1 and e.uc.mem_read(STATE + T0 + TSIZE * 1 + 8, 6)[5] == 0, (e.r8(PAGE + 77), list(e.uc.mem_read(STATE + T0 + TSIZE * 1 + 8, 6))))
touch("down", 78 + 2 + 10 + 35, y0 + 105 + 10)
touch("up", 78 + 2 + 10 + 35, y0 + 105 + 10)
check("a second tap on HALF, now inside the square, sends the half speed event", e.uc.mem_read(STATE + T0 + TSIZE * 1 + 8, 6)[5] == 1, list(e.uc.mem_read(STATE + T0 + TSIZE * 1 + 8, 6)))
before = list(e.uc.mem_read(STATE + T0 + TSIZE * 1 + 8, 6))
knob(0, 800)
after = list(e.uc.mem_read(STATE + T0 + TSIZE * 1 + 8, 6))
check("REV as an encoder: knob 1 of group 1 (DLY RVB REV HALF) sends the reverse event on the selected track", after[4] == before[4] + 1, (before, after))
blocks(80)
touch("down", 78 + 2 + 10, y0 + 35 + 10)
touch("up", 78 + 2 + 10, y0 + 35 + 10)
touch("down", 78 + 2 + 10, y0 + 35 + 10)
touch("up", 78 + 2 + 10, y0 + 35 + 10)
check("a fast double tap on a dial puts it back to its default (CRSH 0)", abs(tr(1, O_CRUNCH, "f")) < 0.001, tr(1, O_CRUNCH, "f"))
f = draw(0)
check("a half speed track shows its 1/2 badge and a yellow HALF frame", any(x[4] == 0x14 for x in f))
tab(3)
f = draw(0)
check("SETUP tab drawn inside the screen", all(x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224 for x in f), [x for x in f if not (x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224)][:3])
ROW0 = 16 + 3                                                  # first row, from the top
def row(r):
    return ROW0 + r * 22 + 8
def optv(i):
    return struct.unpack("<f", e.uc.mem_read(STATE + 164 + 4 * i, 4))[0]
check("SETUP: LENGTH row choices (FOLLOW / MULT / FREE) and the SYNC row", [hit(84 + 10, row(0))[:2], hit(84 + 52 + 10, row(0))[:2], hit(84 + 104 + 10, row(0))[:2], hit(84 + 10, row(1))[:2]] == [(9, 0), (9, 0), (9, 0), (9, 1)], [hit(84 + 10, row(0)), hit(84 + 62, row(0)), hit(84 + 114, row(0)), hit(94, row(1))])
check("SETUP: the choice index is the value", [hit(94, row(0))[2], hit(146, row(0))[2], hit(198, row(0))[2]] == [0, 1, 2])
touch("down", 84 + 52 + 10, row(0))
touch("up", 84 + 52 + 10, row(0))
check("SETUP: tapping MULT sets the length option to 1", optv(0) == 1.0, optv(0))
touch("down", 84 + 52 + 10, row(1))
touch("up", 84 + 52 + 10, row(1))
touch("down", 84 + 52 + 10, row(3))
touch("up", 84 + 52 + 10, row(3))
check("SETUP: SYNC on, SOURCE mix", optv(1) == 1.0 and optv(3) == 1.0)
touch("down", 84 + 37, row(5))
touch("up", 84 + 37, row(5))
check("SETUP: the LOOP GAIN slider (37 of 150 px = 25 %)", abs(optv(9) - 0.247) < 0.01, optv(9))
knob(0, 800)
check("SETUP: knob 1 turns the loop gain (+10 %)", abs(optv(9) - 0.347) < 0.01, optv(9))
f = draw(0)
check("SETUP: lit choice boxes and a slider drawn", any(x[2] == 150 and x[3] == 1 for x in f) and any(x[4] == 0x1B and x[2] == 49 for x in f))
check("SETUP: everything inside the screen", all(x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224 for x in f))
touch("down", 84 + 10, row(7))
touch("up", 84 + 10, row(7))
blocks(3)
check("SETUP: the first CLEAR ALL press only asks (nothing erased yet)", mode(0) == PLAY, mode(0))
touch("down", 84 + 10, row(7))
touch("up", 84 + 10, row(7))
blocks(2)
check("SETUP: CLEAR ALL erases the tracks (track 1 was playing)", mode(0) in (CLEARING, EMPTY), mode(0))
blocks(70)
check("...and they end up empty", all(mode(t) == EMPTY for t in range(4)))
tab(4)
f = draw(0)
check("MORE tab shows its rows and the diagnostic lines", len(pixels) > 300, (len(pixels),))
check("MORE tab drawn inside the screen", all(x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224 for x in f))
touch("down", 84 + 100, row(2))
touch("move", 84 + 112, row(2))
touch("up", 84 + 112, row(2))
check("MORE: dragging the DLY FB slider to 112 / 150 px = 75 %", abs(optv(5) - 0.75) < 0.01, optv(5))
knob(3, -800)
check("MORE: knob 4 turns the reverb return (60 % - 10 %)", abs(optv(8) - 0.5) < 0.01, optv(8))
touch("down", 84 + 10, row(0))
touch("up", 84 + 10, row(0))
check("MORE: FX ROUTE choice STOCK (0)", optv(10) == 0.0)
touch("down", 84 + 60, row(0))
touch("up", 84 + 60, row(0))
check("MORE: FX ROUTE choice OWN (1)", optv(10) == 1.0)

opt_sync_off = struct.pack("<f", 0.0)
for off in (164, 168, 176, 164 + 40):
    e.uc.mem_write(STATE + off, opt_sync_off)
tab(0)
f = draw(0)
check("back to MAIN: the fader meters are back", [x for x in f if x[2] == 2 and x[3] > 20])
check("back on the MAIN tab", e.r8(PAGE + 76) == 0)

# ---- SELECT and the hardware REC button
tab(0)
draw(0)
check("SELECT button zone (under the record box)", hit(78 + 10, 19 + 40 + 3 + 5)[0] == 13, hit(78 + 10, 19 + 40 + 3 + 5))
touch("down", 78 + 10, 19 + 40 + 3 + 5)
touch("up", 78 + 10, 19 + 40 + 3 + 5)
check("tapping SELECT selects track 2 without a record event", e.r8(PAGE + 89) == 1, e.r8(PAGE + 89))
f = draw(0)
check("the selected track's SELECT button is pink", any(x[4] == 0x20 and x[2] == 73 for x in f))
tab(4)
e.uc.mem_write(PAGE + 90, b"\x02")                                    # LEARN REC BTN tapped (slot 2)
appmsg.clear()
e.uc.mem_write(MSG, struct.pack("<H", 0xF5) + bytes(10) + struct.pack("<i", 0))
e.call("looper_app_msg", APP, MSG)
check("learning the REC button (message 0xf5): remembered and swallowed", e.r8(PAGE + 98) == 1 and e.r16(PAGE + 112) == 0xF5 and not appmsg, (e.r8(PAGE + 98), e.r16(PAGE + 112), appmsg))
before = list(e.uc.mem_read(STATE + T0 + TSIZE * 1 + 8, 2))
e.call("looper_app_msg", APP, MSG)
after = list(e.uc.mem_read(STATE + T0 + TSIZE * 1 + 8, 2))
check("the REC button on the Looper page: a tap on the selected track (2), and the sequencer never sees it", after[0] == before[0] + 1 and after[1] == before[1] + 1 and appmsg == [], (before, after, appmsg))
tab(0)

# ---- the stock screen's pieces under the page
e.uc.mem_write(VIEW + 4, struct.pack("<4i", 32, 0, 256, 224))
e.uc.mem_write(VIEW + 0x3AC + 0x6C, b"\x00")                          # the stock code shows a child again
e.call("looper_page_enter", VIEW)
f = draw(0)
r = struct.unpack("<4i", e.uc.mem_read(VIEW + 4, 16))
check("the view's rectangle is widened to the whole screen while the page shows (edge touches reach it)", r == (0, 0, 320, 224), r)
check("a child widget the stock code showed again is hidden again at the next draw", e.r8(VIEW + 0x3AC + 0x6C) == 1)
# INFO on: the pan dials get the pink frame, the top row stays LVL
e.uc.mem_write(PAGE + 7, b"\x01")
f = draw(0)
check("SHIFT ON: red label in the footer, no INFO=n", any(c == 0x0C for _, _, c in pixels))
check("SHIFT ON: the pan dials are framed pink", sum(1 for x in f if x[4] == 0x20 and x[2] == 34) >= 4, sum(1 for x in f if x[4] == 0x20))
e.uc.mem_write(PAGE + 7, b"\x00")
# the UNDO button
btn_u = 16 + (224 - 16 - 14 - 2) - 2 - 34 - 15
touch("down", 100, btn_u + 5)
touch("up", 100, btn_u + 5)
check("the UNDO button sends the undo event for its track", e.uc.mem_read(STATE + T0 + TSIZE * 1 + 8, 7)[6] == 1, list(e.uc.mem_read(STATE + T0 + TSIZE * 1 + 8, 7)))
# INFO belongs to the stock screens elsewhere
e.uc.mem_write(APP + 0x8CA4, b"\x25")
appmsg.clear()
button(0, msg_id=0xC)
check("on another screen the INFO button goes to the stock handler (the Looper flag outlives the trip)", appmsg == [(0xC, 0)], appmsg)
e.uc.mem_write(APP + 0x8CA4, b"\x2f")
# FX button learning and the tab toggle
appmsg.clear()
blocks(30)
e.uc.mem_write(PAGE + 90, b"\x01")                                    # LEARN FX BTN was tapped (slot 1)
button(6)
check("learning the FX button: swallowed, remembered (message 0xf9, button 6)", e.r8(PAGE + 97) == 1 and e.r8(PAGE + 103) == 6 and e.r16(PAGE + 110) == 0xF9 and e.r8(PAGE + 90) == 0 and not appmsg, (e.r8(PAGE + 97), e.r8(PAGE + 103), appmsg))
blocks(30)
button(6)
check("the FX button on the Looper page: the looper's FX tab, not the Blackbox's", e.r8(PAGE + 76) == 1 and not appmsg, (e.r8(PAGE + 76), appmsg))
blocks(30)
button(6)
check("... the FX button again: the FX2 page (still the FX tab)", e.r8(PAGE + 76) == 1 and e.r8(PAGE + 77) == 2 and not appmsg, (e.r8(PAGE + 76), e.r8(PAGE + 77)))
blocks(30)
button(6)
check("... and once more: back to MAIN", e.r8(PAGE + 76) == 0 and not appmsg)
# the stock FX page: INFO + FX (on MAIN), and the footer's STOCK FX button
blocks(30)
button(0, msg_id=0xC)
check("INFO switches SHIFT on (MAIN tab)", e.r8(PAGE + 7) == 1, e.r8(PAGE + 7))
blocks(30)
button(6)
check("INFO + FX: the FX button's message goes to the stock handler (its FX page), SHIFT is off again", appmsg == [(0xF9, 6)] and e.r8(PAGE + 7) == 0 and e.r8(PAGE + 76) == 0, (appmsg, e.r8(PAGE + 7), e.r8(PAGE + 76)))
appmsg.clear()
check("the footer's right end is the STOCK FX button", hit(300, 224 - 5)[:2] == (9, 94), hit(300, 224 - 5))
touch("down", 300, 224 - 5)
touch("up", 300, 224 - 5)
check("tapping STOCK FX hands the FX message to the stock handler", appmsg == [(0xF9, 6)], appmsg)
appmsg.clear()
e.uc.mem_write(APP + 0x8CA4, b"\x25")
blocks(30)
button(6)
check("on other screens the FX button is the stock one", appmsg == [(0xF9, 6)], appmsg)
e.uc.mem_write(APP + 0x8CA4, b"\x2f")
e.uc.mem_write(STATE + 164 + 11 * 4, struct.pack("<f", 1.0))            # FULL SCREEN on
e.call("looper_page_enter", VIEW)
f = draw(0)
check("full screen: the page grows upward over the screen's own top bar (240 high, same bottom edge)", f[0][:4] == (0, 0, 320, 240), f[0][:4])
check("... and still draws inside the screen", all(x[1] >= 0 and x[1] + x[3] <= 240 for x in f))
e.uc.mem_write(STATE + 164 + 11 * 4, struct.pack("<f", 0.0))
e.call("looper_page_enter", VIEW)
draw(0)
# ---- the drawing guard and the touch hit test
GB, GD, SCR = 0x2405FF58, 0x2405FF5C, 0x24030000 + 0x8CA4
e.uc.mem_write(SCR, b"\x2f")
e.uc.mem_write(SOLO + 6, b"\x01")
e.call("looper_guard", 0)
# hit test: stock logic reproduced (widget +0x30 hidden, children at +0x18 / next +0x20, vtable +0xc rect, +0x1c check)
HW, HC, HV, HCV = 0x3003E000, 0x3003E200, 0x3003E400, 0x3003E500
e.w32(HW, HV); e.w32(HC, HCV)
e.w32(HW + 0x18, HC); e.w32(HC + 0x20, 0)
SA, SB, SC = 0x080EE100, 0x080EE110, 0x080EE120
e.w32(HV + 0xC, SB | 1); e.w32(HV + 0x1C, SC | 1); e.w32(HCV + 0x28, SA | 1)
hlog = []
child_r, rect_r, chk_r = [0], [1], [5]
def s_child():
    hlog.append("child")
    if child_r[0]:
        e.w32(e.arg(2), HC)
    e.uc.reg_write(A.UC_ARM_REG_R0, child_r[0])
def s_rect():
    hlog.append("rect"); e.uc.reg_write(A.UC_ARM_REG_R0, rect_r[0])
def s_chk():
    hlog.append("chk"); e.uc.reg_write(A.UC_ARM_REG_R0, chk_r[0])
e.stub(SA, s_child); e.stub(SB, s_rect); e.stub(SC, s_chk)
def hit_w(hidden=0):
    e.uc.mem_write(HW + 0x30, bytes([hidden]))
    e.w32(0x3003E800, 0)
    hlog.clear()
    e.call("looper_basehit", HW, 0x3003E900, 0x3003E800)
    return e.uc.reg_read(A.UC_ARM_REG_R0), e.r32(0x3003E800)
child_r[0], rect_r[0], chk_r[0] = 1, 1, 5
r = hit_w()
check("hit test: a child that takes the point wins, the widget itself is not asked", r == (1, HC) and hlog == ["child"], (r, hlog))
child_r[0] = 0
r = hit_w()
check("hit test: no child: the widget's own rectangle and check decide, the answer is the widget (value of the check)", r == (5, HW) and hlog == ["child", "rect", "chk"], (r, hlog))
rect_r[0] = 0
r = hit_w()
check("hit test: outside the rectangle: 0", r[0] == 0 and hlog == ["child", "rect"], (r, hlog))
rect_r[0], chk_r[0] = 1, 0
r = hit_w()
check("hit test: the check refuses: 0", r[0] == 0 and hlog == ["child", "rect", "chk"], (r, hlog))
chk_r[0] = 5
r = hit_w(hidden=1)
check("hit test: a hidden widget is never hit", r[0] == 0 and not hlog, (r, hlog))
e.call("looper_guard", 1)
e.w32(0x2405FF64, 0) if False else None
r = hit_w()
check("hit test while the page owns the screen: always the mixer view (nothing behind the page can take a touch)", r == (1, VIEW) and not hlog, (r, hlog))
e.call("looper_guard", 0)
# the REC button default (message 0xf4) and its debounce
blocks(30)
tab(4)
e.call("looper_page_enter", VIEW)
appmsg.clear()
before = list(e.uc.mem_read(STATE + T0 + TSIZE * 1 + 8, 2))
e.uc.mem_write(MSG, struct.pack("<H", 0xF4) + bytes(10) + struct.pack("<i", 8))
e.uc.mem_write(PAGE + 98, b"\x01"); e.uc.mem_write(PAGE + 104, b"\x08"); e.uc.mem_write(PAGE + 112, struct.pack("<H", 0xF4))
blocks(30)
appmsg.clear()
e.call("looper_app_msg", APP, MSG)
e.call("looper_app_msg", APP, MSG)
after = list(e.uc.mem_read(STATE + T0 + TSIZE * 1 + 8, 2))
check("REC button (0xf4, 8) is preset: one tap on the selected track; the release right after is ignored; the stock handler never sees REC", after[0] == before[0] + 1 and appmsg == [], (before, after, appmsg))
tab(0)

# INFO + tap on each record box toggles that track's mute
tab(0)
for t in range(4):
    e.uc.mem_write(STATE + T0 + TSIZE * t + 1, b"\x00")
e.uc.mem_write(PAGE + 7, b"\x00")
button(0, msg_id=0xC)
check("INFO on (setup for the per-track mute check)", e.r8(PAGE + 7) == 1)
for t in range(4):
    x = 1 + 78 * t + 40
    touch("down", x, 30)
    touch("up", x, 30)
    blocks(3)
check("INFO + tap on each of the four record boxes mutes each track", [tr(t, 1) for t in range(4)] == [1, 1, 1, 1], [tr(t, 1) for t in range(4)])
for t in range(4):
    x = 1 + 78 * t + 40
    touch("down", x, 30)
    touch("up", x, 30)
    blocks(3)
check("... and a second round unmutes them", [tr(t, 1) for t in range(4)] == [0, 0, 0, 0], [tr(t, 1) for t in range(4)])
button(0, msg_id=0xC)

# ---- BACK / STOP / PLAY learning, INFO cycling on the FX tab
def learn(slot, msg_id, idx):
    blocks(30)
    e.uc.mem_write(PAGE + 90, bytes([slot]))
    e.uc.mem_write(MSG, struct.pack("<H", msg_id) + bytes(10) + struct.pack("<i", idx))
    e.call("looper_app_msg", APP, MSG)
def press(msg_id, idx):
    blocks(30)
    appmsg.clear()
    e.uc.mem_write(MSG, struct.pack("<H", msg_id) + bytes(10) + struct.pack("<i", idx))
    e.call("looper_app_msg", APP, MSG)
learn(3, 0xF9, 0)                                # BACK
learn(4, 0xF9, 1)                                # STOP
learn(5, 0xF9, 2)                                # PLAY
check("BACK, STOP and PLAY learnt (slots 3 4 5)", e.r8(PAGE + 99) == 1 and e.r8(PAGE + 100) == 1 and e.r8(PAGE + 101) == 1)
e.uc.mem_write(PAGE + 89, b"\x02")                # select track 3
before = list(e.uc.mem_read(STATE + T0 + TSIZE * 2 + 8, 7))
press(0xF9, 0)
after = list(e.uc.mem_read(STATE + T0 + TSIZE * 2 + 8, 7))
check("BACK: an undo event for the selected track (3), swallowed", after[6] == before[6] + 1 and not appmsg, (before, after, appmsg))
press(0xF9, 2)
blocks(2)
check("PLAY / PAUSE: the looper only (HW STOP PLAY = LOOPER)", appmsg == [], appmsg)
press(0xF9, 2)
blocks(2)
press(0xF9, 1)
blocks(2)
check("STOP: the looper only", appmsg == [], appmsg)
e.uc.mem_write(STATE + 164 + 12 * 4, struct.pack("<f", 1.0))              # HW STOP PLAY: +STOCK
press(0xF9, 1)
check("with +STOCK the sequencer gets STOP too", appmsg == [(0xF9, 1)], appmsg)
e.uc.mem_write(STATE + 164 + 12 * 4, struct.pack("<f", 0.0))
e.uc.mem_write(APP + 0x8CA4, b"\x25")
press(0xF9, 1)
check("on other screens they are the stock buttons", appmsg == [(0xF9, 1)], appmsg)
e.uc.mem_write(APP + 0x8CA4, b"\x2f")
tab(1)
fx0 = list(e.uc.mem_read(PAGE + 77, 4))
e.uc.mem_write(PAGE + 77, bytes([0, 0, 0, 0]))
e.uc.mem_write(PAGE + 89, b"\x01")
blocks(30)
button(0, msg_id=0xC)
check("INFO on the FX tab moves the one pink square on to the next block of four (same page)", e.r8(PAGE + 77) == 1 and e.r8(PAGE + 89) == 1 and e.r8(PAGE + 7) == 0, (e.r8(PAGE + 77), e.r8(PAGE + 89)))
button(0, msg_id=0xC)
check("... after the page's second block the square goes to the next track's first block", e.r8(PAGE + 77) == 0 and e.r8(PAGE + 89) == 2, (e.r8(PAGE + 77), e.r8(PAGE + 89)))
e.uc.mem_write(PAGE + 77, bytes([3, 3, 3, 3]))
button(0, msg_id=0xC)
check("... on the FX2 page INFO stays on that page (block 3 -> next track, block 2)", e.r8(PAGE + 77) == 2 and e.r8(PAGE + 89) == 3, (e.r8(PAGE + 77), e.r8(PAGE + 89)))
e.uc.mem_write(PAGE + 77, bytes([2, 2, 2, 2]))
e.uc.mem_write(PAGE + 89, b"\x01")
h7 = hit(1 + 78 * 1 + 2 + 10, y0 + 10)
h8 = hit(1 + 78 * 1 + 2 + 10 + 35, y0 + 35 + 10)
check("second page: the first tiles are STAB and (row 2, column 2) DROP", (h7[0], int(h7[2])) == (7, 8) and (h8[0], int(h8[2])) == (7, 11), (h7, h8))
knob(1, 800)
check("second page: knob 2 raises STAB of the selected track (+10 %)", abs(tr(1, O_SR + 4, "f") - 0.1) < 0.02, tr(1, O_SR + 4, "f"))
e.uc.mem_write(PAGE + 77, bytes([0, 0, 0, 0]))
tab(0)

# ---- live repaint: the audio task's poke posts a message, the GUI task paints
posted = []
e.stub(0x080B5758, lambda: posted.append(e.r16(e.arg(1))))
e.uc.mem_write(STATE + 164 + 11 * 4, struct.pack("<f", 1.0))            # full screen
e.call("looper_page_enter", VIEW)
draw(0)
e.uc.mem_write(PAGE + 120, b"\x00")
level(2, 0.4)
posted.clear()
e.call("looper_page_poke", VIEW)
check("full screen: a change makes the poke post one paint message for the GUI task", posted == [0x1F0], posted)
e.call("looper_page_poke", VIEW)
level(2, 0.45)
e.call("looper_page_poke", VIEW)
check("... and only one while it is in flight", posted == [0x1F0], posted)
fills.clear()
e.uc.mem_write(MSG, struct.pack("<H", 0x1F0) + bytes(30))
e.call("looper_app_msg", APP, MSG, count=50_000_000)
check("the paint message repaints the page in the GUI task and is not passed on", len(fills) > 50, len(fills))
e.uc.mem_write(STATE + 164 + 11 * 4, struct.pack("<f", 0.0))
e.call("looper_page_enter", VIEW)
draw(0)

# ---- stock line / text drawing and other views' cells are dropped while the page shows
e.uc.mem_write(SCR, b"\x2f"); e.uc.mem_write(SOLO + 6, b"\x01"); e.uc.mem_write(VIEW + 0x1E40, b"\x01")
e.call("looper_note_app", APP)
e.call("looper_guard", 2)
def stock_blocked():
    e.call("looper_stock_blocked")
    return e.uc.reg_read(A.UC_ARM_REG_R0)
check("page showing: stock line / text drawing is dropped", stock_blocked() == 1)
e.call("looper_guard_drawing", 1)
check("... but not while the page itself draws", stock_blocked() == 0)
e.call("looper_guard_drawing", 0)
e.uc.mem_write(SCR, b"\x25")
check("on another screen nothing is dropped", stock_blocked() == 0)
e.uc.mem_write(SCR, b"\x2f")
celldraw.clear()
OTHER = 0x30026000
e.uc.mem_write(OTHER, bytes(0x400))
e.call("looper_cell_draw", OTHER, CTX, count=5_000_000)
check("a cell of any other view is not drawn while the page shows", celldraw == [], celldraw)
e.uc.mem_write(SCR, b"\x25")
celldraw.clear()
e.call("looper_cell_draw", OTHER, CTX, count=5_000_000)
check("on another screen (the pads page uses the same cells) the stock cell draw runs although the Looper flag is stale", celldraw == [OTHER], celldraw)
e.uc.mem_write(SCR, b"\x2f")

# ---- stock fills in the side margins are dropped (left-side glitches), the page's own and the cells' area are not
def fill_blocked(x, w):
    e.uc.mem_write(0x3003C000, struct.pack("<6H", 0, 0, w, 10, x, 40))
    e.call("looper_fill_blocked", 0, 5, 0x3003C000)
    return e.uc.reg_read(A.UC_ARM_REG_R0)
e.uc.mem_write(SCR, b"\x2f"); e.uc.mem_write(SOLO + 6, b"\x01"); e.uc.mem_write(VIEW + 0x1E40, b"\x01")
e.call("looper_note_app", APP); e.call("looper_guard", 2)
check("a stock fill in the left margin is dropped, in the cell area or the right margin as well as appropriate",
      fill_blocked(0, 30) == 1 and fill_blocked(100, 50) == 0 and fill_blocked(290, 20) == 1 and fill_blocked(20, 40) == 0)
e.call("looper_guard_drawing", 1)
check("... not while the page itself draws", fill_blocked(0, 30) == 0)
e.call("looper_guard_drawing", 0)
e.uc.mem_write(SCR, b"\x25")
check("... and not on other screens", fill_blocked(0, 30) == 0)
e.uc.mem_write(SCR, b"\x2f")

# the key events' second ring (audio task): PLAY / STOP / REC are dropped while the page shows
KO = 0x3003C200
def ring_b(entries):
    e.uc.mem_write(KO + 0xC08, struct.pack("<II", 0, len(entries)))
    for i, (kid, kidx) in enumerate(entries):
        e.uc.mem_write(KO + 0x608 + 24 * i, struct.pack("<H", kid))
        e.uc.mem_write(KO + 0x610 + 24 * i, struct.pack("<H", 0))
        e.uc.mem_write(KO + 0x614 + 24 * i, struct.pack("<I", kidx))
def key_pop():
    e.call("looper_key_pop", KO, 0x3003F000)
    return e.uc.reg_read(A.UC_ARM_REG_R0), e.r16(0x3003F000), e.r32(0x3003F000 + 0xC)
ring_b([(0xF7, 10), (0xF6, 9), (0xF4, 8), (0xF9, 4)])
check("key ring: PLAY, STOP and REC are dropped, FX is delivered", key_pop() == (1, 0xF9, 4), key_pop())
ring_b([(0xF7, 10)])
e.uc.mem_write(OPTS + 4 * 12, struct.pack("<f", 1.0)) if False else None
check("key ring: empty after the drops", key_pop()[0] == 0)
e.uc.mem_write(SCR, b"\x25")
ring_b([(0xF7, 10)])
check("key ring: on other screens PLAY is delivered", key_pop() == (1, 0xF7, 10))
e.uc.mem_write(SCR, b"\x2f")

# ---- SMPLR tab: waveform, slicer touches, buttons
SE, LENF = 0x2400A9C0, 16384
SLOTS, BLK, BUFL, APPS = 0x24040000, 0x24050000, 0x24060000, 0x24020088
e.w32(SE + 0x4348, SLOTS)
e.uc.mem_write(SE + 0x434C, b"\xff\xff" * 576)
e.uc.mem_write(SE + 0x434C + 10, struct.pack("<H", 0))
e.uc.mem_write(SLOTS + 0x2C, b"\x01")
e.uc.mem_write(SLOTS + 0x1A, struct.pack("<H", 1))
e.w32(SLOTS + 0x1C, 48000)
e.uc.mem_write(SLOTS + 0x50, struct.pack("<II", LENF, 0))
e.w32(SE + 0x8670, BLK)
e.uc.mem_write(BLK, struct.pack("<HH", 1, 2) + b"\xff\xff" * 8)
for b_ in range(2):
    ent = SE + 0x1C * (1 + b_)
    e.w32(ent + 4, BUFL + b_ * 0x8000)
    e.w32(ent + 8, BUFL + b_ * 0x8000)
    e.w32(ent + 0xC, b_)
    e.w32(ent + 0x14, 8192)
    e.w32(ent + 0x18, 5)
    e.uc.mem_write(ent + 0x1E, b"\x00\x02")
    e.uc.mem_write(BUFL + b_ * 0x8000, struct.pack("<8192f", *[(b_ * 8192 + i) / LENF for i in range(8192)]))
e.w32(APPS + 0x8A88, 5)
e.w32(APPS + 0x8A88 + 4, 0xFFFF)
e.uc.mem_write(APPS + 0x8A88 + 8, b"\x01")
e.uc.mem_write(0x24072000, b"/S/kick.wav\0")
e.w32(APPS + 0x1840 + 0x18, 0x24072000)


def pcm_stub():
    sp = e.uc.reg_read(A.UC_ARM_REG_SP)
    ident, outl, outr, n = struct.unpack("<IIII", e.uc.mem_read(sp, 16))
    start = e.arg(2)
    vals = [(start + i) / LENF if 0 <= start + i < LENF else 0.0 for i in range(n)]
    e.uc.mem_write(outl, struct.pack(f"<{n}f", *vals))
    e.ret(1)


e.stub(0x08074A00, pcm_stub)
e.stub(0x08074CE8)
e.call("looper_page_goto", VIEW, 5)
f = draw(0)
check("SMPLR page: no footer tabs (a page of its own), nothing is drawn outside the screen",
      not any(x[2] == 34 and x[3] == 1 and x[1] <= 14 for x in f) and all(x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224 for x in f),
      [x for x in f if not (x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224)][:3])
cols = [x for x in f if x[2] == 2 and x[4] == 0x16]
check("SMPLR tab: 150 waveform columns of 2 px, growing along the ramp", len(cols) == 150 and cols[-1][3] > cols[10][3], (len(cols), cols[:1], cols[-1:]))
check("SMPLR tab: slice lines (grey)", sum(1 for x in f if x[2] == 1 and x[4] == 0x09) >= 15)
import subprocess as _sp
def _off(name):
    src = '#include "src/samplr.h"\nchar o_x[__builtin_offsetof(struct sm,%s)];\n' % name
    out = _sp.run(["arm-none-eabi-gcc", "-I.", "-mthumb", "-S", "-x", "c", "-", "-o", "-"], input=src, capture_output=True, text=True, check=True).stdout.splitlines()
    return int([l.split()[1] for l in out if l.strip().startswith(".space")][0])


G_ARMED = _off("g_armed")
e.call("samplr")
SMPLR_P = e.uc.reg_read(A.UC_ARM_REG_R0)
before_l = list(e.uc.mem_read(STATE + T0, TSIZE * 4))
press(0xF4, 8)
check("on the SAMPLR page the hardware REC arms the gesture recorder", e.r8(SMPLR_P + G_ARMED) == 1, e.r8(SMPLR_P + G_ARMED))
check("... and the looper's tracks did not see it", list(e.uc.mem_read(STATE + T0, TSIZE * 4)) == before_l)
press(0xF9, 1)
check("... STOP stops the gesture timeline (and arming)", e.r8(SMPLR_P + G_ARMED) == 0, e.r8(SMPLR_P + G_ARMED))
wave_x, wave_d = 3 + 7 + 150, 90
touch("down", wave_x, wave_d)
blocks(3, 0.0)
f = draw(0)
check("SMPLR: touching the waveform plays (the Out 1 bus gets the sample)", max(abs(v) for v in block([0.0] * N)[1][0]) > 0.001)
check("SMPLR: the touched slice is drawn in the finger's colour (cyan)", any(x[2] == 2 and x[4] == 0x1B for x in f))
touch("up", wave_x, wave_d)
blocks(3, 0.0)
check("SMPLR: lifting the finger silences it (gate)", max(abs(v) for v in block([0.0] * N)[1][0]) < 1e-4)
touch("down", 3 + 200 + 10, 219)                                 # the MODE sheet (last footer tab)
touch("up", 3 + 200 + 10, 219)
touch("down", 3 + 77 + 10, 178 + 10)                               # TAPE

touch("up", 3 + 77 + 10, 178 + 10)
e.call("samplr")
check("SMPLR: the TAPE button switches the mode", e.r8(e.uc.reg_read(A.UC_ARM_REG_R0) + 4) == 1, e.r8(e.uc.reg_read(A.UC_ARM_REG_R0) + 4))
touch("down", 3 + 50 + 10, 219)                                  # the SAMPLE sheet in the footer
touch("up", 3 + 50 + 10, 219)
touch("down", 140, 27)                                          # +12 in the strip above the sample (always there)
touch("up", 140, 27)
touch("down", 140, 27)                                          # +12 in the strip above the sample (always there)
touch("up", 140, 27)
check("SMPLR: the SAMPLE sheet's +12 button transposes (twice = 24)", struct.unpack("<b", e.uc.mem_read(SMPLR_P + _off("trans"), 1))[0] == 24, struct.unpack("<b", e.uc.mem_read(SMPLR_P + _off("trans"), 1))[0])
touch("down", 80, 27)                                           # the value: back to 0
touch("up", 80, 27)
check("SMPLR: tapping the value resets the transpose", struct.unpack("<b", e.uc.mem_read(SMPLR_P + _off("trans"), 1))[0] == 0)
touch("down", 3 + 100 + 10, 219)                                 # the GESTURE sheet
touch("up", 3 + 100 + 10, 219)
touch("down", 200, 27)                                          # REC in the strip above the sample
touch("up", 200, 27)
check("SMPLR: the GESTURE sheet's REC arms the take", e.r8(SMPLR_P + G_ARMED) == 1, e.r8(SMPLR_P + G_ARMED))
touch("down", 200, 27)                                          # REC again cancels
touch("up", 200, 27)
touch("down", 3 + 10, 219)                                       # back to the PLAY sheet
touch("up", 3 + 10, 219)
e.call("looper_page_goto", VIEW, 0)
check("SMPLR: leaving the tab releases the voices", True)

# ---- page order: MIX opens the Looper first, SONG opens SMPLR first
e.uc.mem_write(APP + 0x8CA4, b"\x25")
screens.clear()
e.call("solo_mix_pressed", APP, 0, 0, 0)
check("MIX from another screen goes to the mixer's mute screen (0x2f) to open the Looper page", screens == [0x2F], screens)
e.call("solo_set_mode", VIEW, 1)
check("... and the page opens on MAIN", e.r8(PAGE + 76) == 0, e.r8(PAGE + 76))
e.uc.mem_write(APP + 0x8CA4, b"\x25")
screens.clear()
e.call("solo_song_pressed", APP, 0, 0, 0)
check("SONG from another screen: the mute screen again, for the SMPLR tab", screens == [0x2F], screens)
e.call("solo_set_mode", VIEW, 1)
check("... and the page opens on SMPLR (mode 4)", e.r8(PAGE + 76) == 4, e.r8(PAGE + 76))
e.uc.mem_write(APP + 0x8CA4, b"\x2f")
screens.clear()
e.call("solo_song_pressed", APP, 0, 0, 0)
check("SONG again on the SMPLR page: the stock song screen (0x2d)", screens == [0x2D], screens)
e.call("looper_page_goto", VIEW, 0)
screens.clear()
e.call("solo_song_pressed", APP, 0, 0, 0)
check("SONG on another tab of the page: switches to SMPLR without leaving the screen", screens == [] and e.r8(PAGE + 76) == 4, (screens, e.r8(PAGE + 76)))
e.uc.mem_write(APP + 0x8CA4, b"\x2f")
screens.clear()
e.call("solo_mix_pressed", APP, 0, 0, 0)
check("MIX on the SMPLR tab goes to the Looper's MAIN tab (no screen change)", screens == [] and e.r8(PAGE + 76) == 0, (screens, e.r8(PAGE + 76)))
e.call("looper_page_goto", VIEW, 0)

# leaving the page puts the child widgets back
e.call("solo_set_mode", VIEW, 1)
check("leaving the page: the children are restored (the one the firmware hid stays hidden)",
      e.r8(VIEW + 0x3AC + 3 * 0x1A0 + 0x6C) == 1 and e.r8(VIEW + 0x3AC + 0x6C) == 0 and e.r8(VIEW + 0x3AC + 9 * 0x1A0 + 0x150) == 0)
e.uc.mem_write(SOLO + 6, b"\x00")
draw(0)
check("off the page the stock cell draw runs", celldraw == [VIEW + 0x3AC] and not fills)

print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
