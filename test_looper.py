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
e.uc.mem_map(0xC0000000, 0x1400000)                  # SDRAM: the pool's buffers
e.uc.mem_map(0x30000000, 0x40000)                    # fake engine + buffers
ENGINE = 0x30000000
INL, INR, PTRS = 0x30010000, 0x30011000, 0x30012000
BUSL, BUSR = 0x30020000, 0x30021000
N = 256
STATE = 0x38800C00
ENTRIES, PER, AREAS = 615, 47, 5
FIRST = ENTRIES - AREAS * PER
T0, TSIZE = 0x3C, 0x30                                # engine state: tracks, track size
LEN, POS, RECN, OK, UNDO_T = 12, 16, 20, 8, 0x2C
REC_DOWN, REC_UP, MUTE_DOWN, MUTE_UP, REVERSE = range(5)
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
    e.call("looper_bus", BUSL, BUSR, N, count=20_000_000)
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
check("boot: the last 235 pool blocks claimed (4 tracks + undo), the others untouched",
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

# --- double tap latches an overdub on track 2 (shorter than one pass of the 60-block loop)
ev(1, REC_DOWN)
block([0.1] * N)
ev(1, REC_UP)
blocks(5, 0.1)
check("first tap: overdubbing, waiting for the second", mode(1) == DUB and tr(1, 3) == 2, (mode(1), tr(1, 3)))
ev(1, REC_DOWN)
block([0.1] * N)
ev(1, REC_UP)
check("second tap within 400 ms: latched", tr(1, 3) == 3 and mode(1) == DUB)
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

# --- a lone short tap takes nothing
ev(2, REC_DOWN)
block([0.3] * N)
ev(2, REC_UP)
blocks(160, 0.3)
check("a lone tap on an empty track: the blip is taken back, track 3 empty again", mode(2) == EMPTY, mode(2))
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
check("pan and level are clamped", abs(tr(0, 0x24, "f") + 1.0) < 1e-6 and tr(0, 0x20, "f") == 1.0)
pan(0, 0.0)

# --- erase with a 4 s hold
ev(0, MUTE_DOWN)
blocks(760)
ev(0, MUTE_UP)
blocks(70)
check("M held 4 s: track 1 erased, loop length free again", mode(0) == EMPTY and st(LEN) == 0, (mode(0), st(LEN)))

# --- 16 s limit
ev(3, REC_DOWN)
blocks(16 * 48000 // N + 2, 0.01)
check("a first take stops itself at 16 s and plays", st(LEN) == 16 * 48000 and mode(3) == PLAY, (st(LEN), mode(3)))
ev(3, REC_UP)
block([0.0] * N)
check("...and the late release does nothing more", mode(3) == PLAY)

# --- the Out 1 mix point (looper_thunk.S)
FP, OBJ, VT, BUFSET, R5 = 0x30030000, 0x30030000 + 0xFC40, 0x30038000, 0x30038100, 0x30038200
e.w32(OBJ, VT)
e.w32(VT + 0x54, 0x08046A15)
e.uc.mem_write(OBJ + 0x1E, struct.pack("<H", 3))
seen = []
e.stub(0x0805F37C, lambda: seen.append(("bus", e.arg(0), e.arg(1))), value=0x30038300)
e.stub(0x0804D8E8, value=N)


def stereo():
    e.w32(e.arg(1), BUSL)
    e.w32(e.arg(2), BUSR)


e.stub(0x0804D9C0, stereo)
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
e.uc.mem_write(APP + 0x8CA4, b"\x2f")
screens = []
e.stub(0x0809EAEC, lambda: screens.append(e.arg(1)))
e.stub(0x080B5E44)
e.call("solo_mix_pressed", APP, 0, 0, 0)
e.call("solo_set_mode", VIEW, 1)
check("MIX in Solo mode goes to the Looper page", screens == [0x2F] and e.r8(SOLO + 6) == 1, (screens, e.r8(SOLO + 6)))

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
# page: 224 high (cells y 0..224), top edge 224, y up, 314 wide from x 3 (320 screen, 3 px margins); 75 px columns.
# Column 0 title bar: x 5, 71 wide, top at d = 3+16+2 = 21
title0 = [x for x in f if x[2] == 71 and x[3] == 16]
check("page painted: background first, then top bar, columns, footer", f[0][:4] == (3, 0, 314, 224) and f[0][4] == 0x02, f[:1])
check("track 1's title bar (empty = grey) sits under the top bar: x 5, y = 224 - 21 - 16 = 187",
      any(x[:4] == (5, 187, 71, 16) and x[4] == 0x10 for x in f), title0[:3])
check("four columns of 79 px pitch across the whole width (x 5, 84, 163, 242)",
      sorted({x[0] for x in f if x[2] == 71 and x[3] == 16}) == [5, 84, 163, 242], sorted({x[0] for x in f if x[2] == 71 and x[3] == 16}))
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
check("a playing track: title bar in its colour (cyan), black text", any(x[:4] == (5, 187, 71, 16) and x[4] == 0x1B for x in f) and any(c == 0xE for _, _, c in pixels))
check("its two bars: a white fader line across them", any(x[4] == 0x0F and x[3] == 2 for x in f))
# levels: bar height follows level
level(0, 0.5)
blocks(2)
f = draw(0)
bars = [x for x in f if x[2] == 16 and x[4] == 0x1B]
check("level 50 %: both bars filled to half of their 100 px (a pair of equal 16 px wide fills)", len(bars) == 2 and bars[0][3] == bars[1][3] == 50, bars)
pan(0, 0.5)
blocks(2)
f = draw(0)
bars = sorted([x for x in f if x[2] == 16 and x[4] == 0x1B], key=lambda x: x[0])
check("pan half right: the left bar is half as tall as the right", len(bars) == 2 and abs(bars[0][3] * 2 - bars[1][3]) <= 2, bars)
pan(0, 0.0)
level(0, 1.0)

# orientation: the same cells with y running downwards, row 3 at the top (smaller y)
cells(y_up=False)
f = draw(0)
check("cells with y down: the page flips to match (title bar at y = 0 + 21)", any(x[:4] == (5, 21, 71, 16) for x in f), [x for x in f if x[2] == 71][:2])
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
check("title / icon area of column 2 is its record box", hit(100, 30)[:2] == (1, 1) and hit(100, 60)[:2] == (1, 1), (hit(100, 30), hit(100, 60)))
z, t, v = hit(190, 100)
check("bars area of column 3 is its fader", z == 2 and t == 2 and 0.0 <= v <= 1.0, (z, t, v))
check("fader value runs 0 at the bottom of the bars to 1 at the top", hit(190, 67)[2] > 0.99 and hit(190, 166)[2] < 0.05, (hit(190, 67)[2], hit(190, 166)[2]))
btn_d = 19 + (224 - 16 - 12 - 2 * 3) - 2 - 18 + 5
check("button row: PAN, REV, MUTE by thirds", [hit(237 + dx, btn_d)[0] for dx in (5, 30, 60)] == [3, 4, 5], [hit(237 + dx, btn_d) for dx in (5, 30, 60)])
check("the top bar, the gaps between columns and the footer are not controls", hit(100, 8)[0] == 0 and hit(77, 100)[0] == 0 and hit(100, 215)[0] == 0)

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
touch("down", 242, btn_d)
touch("up", 242, btn_d)
check("PAN: selects track 4's knob for the pan", e.r8(PAGE + 6) == 0b1000, e.r8(PAGE + 6))
touch("down", 267, btn_d)
touch("up", 267, btn_d)
check("REV: reverse event", evs(3)[4] == 1, evs(3))
touch("down", 297, btn_d)
touch("up", 297, btn_d)
check("MUTE: down and up", evs(3)[2:4] == [1, 1], evs(3))
touch("down", 190, 165)
check("fader: touching near the bottom of the bars sets a low level", tr(2, 0x20, "f") < 0.1, tr(2, 0x20, "f"))
touch("move", 190, 68)
check("fader: dragging to the top sets level 1", abs(tr(2, 0x20, "f") - 1.0) < 0.02, tr(2, 0x20, "f"))
touch("up", 190, 67)
check("the page's touches never reach the stock mixer handlers", len(stock) == stock_clear, stock)

# knobs: message 0x32 to the view
viewmsg = []
e.stub(0x080B5C70, lambda: viewmsg.append(e.r16(e.arg(1))))
MSG = 0x2403A000


def knob(i, counts, msg_id=0x32):
    e.uc.mem_write(MSG, struct.pack("<H", msg_id) + bytes(10) + struct.pack("<h", i) + bytes(2) + struct.pack("<h", counts))
    e.call("looper_view_msg", VIEW, MSG)


level(0, 0.5)
knob(0, -800)
check("knob 1 turns track 1's fader (800 counts = 10 %)", abs(tr(0, 0x20, "f") - 0.4) < 1e-5, tr(0, 0x20, "f"))
knob(3, 1600)
check("knob 4 turns track 4's pan while PAN is selected", abs(tr(3, 0x24, "f") - 0.4) < 1e-5, tr(3, 0x24, "f"))
knob(0, 100, msg_id=0x63)
check("other view messages go to the stock handler", viewmsg == [0x63], viewmsg)

# INFO
appmsg = []
e.stub(0x080A2E60, lambda: appmsg.append((e.r16(e.arg(1)), e.r32(e.arg(1) + 0xC))))


def button(idx, msg_id=0xF9):
    e.uc.mem_write(MSG, struct.pack("<H", msg_id) + bytes(10) + struct.pack("<i", idx))
    e.call("looper_app_msg", APP, MSG)


check("INFO not learned yet at boot", e.r8(PAGE + 8) == 0xFF, e.r8(PAGE + 8))
button(5)
check("MIX always reaches the stock handler", appmsg == [(0xF9, 5)], appmsg)
button(4)
check("the first other button is learned as INFO and swallowed", e.r8(PAGE + 8) == 4 and appmsg == [(0xF9, 5)] and e.r8(PAGE + 7) == 1, (e.r8(PAGE + 8), appmsg, e.r8(PAGE + 7)))
button(2)
check("other buttons pass through once INFO is known", appmsg[-1] == (0xF9, 2), appmsg)
knob(1, 800)
check("INFO on: knob 2 turns the pan, not the level", abs(tr(1, 0x24, "f") - 0.2) < 1e-5 and tr(1, 0x20, "f") == 1.0, (tr(1, 0x24, "f"), tr(1, 0x20, "f")))
touch("down", 100, 30)
touch("up", 100, 30)
check("INFO on: a record box tap is a MUTE press, not a REC press", evs(1)[2:4] == [1, 1] and evs(1)[:2] == [1, 1], evs(1))
button(4)
check("INFO again: off", e.r8(PAGE + 7) == 0)
button(4)
button(4, msg_id=0xFA)
check("a quick release keeps INFO on (tap = latch)", e.r8(PAGE + 7) == 1)
button(4)
blocks(100)
button(4, msg_id=0xFA)
check("a release after 0.45 s or more ends INFO (held = momentary)", e.r8(PAGE + 7) == 0, e.r8(PAGE + 7))
e.uc.mem_write(SOLO + 6, b"\x00")
button(4)
check("off the page every button goes to the stock handler (INFO included)", appmsg[-1] == (0xF9, 4), appmsg[-1])
e.uc.mem_write(SOLO + 6, b"\x01")

# footer, signature
f = draw(0)
check("the footer shows the learned INFO button", any(c == 0x09 or c == 0x14 for _, _, c in pixels))

e.call("looper_page_enter")
level(1, 0.3)
e.call("looper_page_poke", VIEW)
check("a change marks every cell dirty", all(e.r8(VIEW + 0x3AC + i * 0x1A0 + 0x17C) == 1 for i in range(16)))
e.uc.mem_write(SOLO + 6, b"\x00")
draw(0)
check("off the page the stock cell draw runs", celldraw == [VIEW + 0x3AC] and not fills)

print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
