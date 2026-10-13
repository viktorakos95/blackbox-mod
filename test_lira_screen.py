"""The LIRA page (src/lira_ui.inc in looper_page.c, solo_pad_pressed in solo.c) under Unicorn: drawn into a recorded frame buffer,
plates / pairs / dials by touch. Same harness as test_looper.py. NOT checked: how it looks, the real touch panel.
"""

import struct
import sys

import unicorn.arm_const as A

from emu import STACK, Emu

e = Emu("out/solo+slice+duck+chord+cond+filter+cpu+od+comp+seqfix+fx2+munchi+looper+lira+cave/BLACKBOX.bin", "out/cave.elf")
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

# ---- the LIRA page ----
e.uc.mem_map(0xC2000000, 0x800000)                                   # the LIRA pool (SDRAM allocation)
regs = []
e.stub(0x0808C0D8)
e.stub(0x0808C12C)
e.stub(0x0804436C, value=0xC2000000)
e.call("interp_register", 0x24060000, 0xF1, 0, 0, 2, 0)
PG = 0x38800F00 if False else None
screens = []
e.stub(0x0809EAEC, lambda: screens.append(e.arg(1)))
e.stub(0x080B5E44)
e.uc.mem_write(APP + 0x8CA4, b"\x02")                                # on the stock pad screen
screens.clear()
e.call("solo_pad_pressed", APP, 0, 0, 0)
check("PAD pressed on the pad screen opens the mixer view (the page)", screens == [0x2F], screens)
e.call("solo_set_mode", VIEW, 1)
e.call("looper_page_tab")
check("... on tab 6, the LIRA page", e.uc.reg_read(A.UC_ARM_REG_R0) == 6, e.uc.reg_read(A.UC_ARM_REG_R0))
f = draw(0)
check("LIRA page painted: background first", f and f[0][:4] == (3, 0, 314, 224) and f[0][4] == 0x02, f[:1])
inside = all(x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224 for x in f)
check("all fills lie inside the screen", inside, [x for x in f if not (x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224)][:4])
check("text inside the screen", pixels and all(3 <= px < 317 and 0 <= py < 224 for px, py, _ in pixels), pixels[:3])
check("eight plates (frames 36 px wide)", len({x[0] for x in f if x[2] == 36 and x[3] == 1}) >= 8, sorted({x[0] for x in f if x[3] == 1})[:12])
check("the active group's four sliders have pink frames (TUNE 1-4)", sum(1 for x in f if x[4] == 0x20) >= 16, sum(1 for x in f if x[4] == 0x20))


def touch(kind, x_rel, d, up=True):
    e.uc.mem_write(PTA, struct.pack("<2i", 3 + x_rel, 224 - d))
    e.call({"down": "solo_touch_down", "move": "solo_touch_move", "up": "solo_touch_up"}[kind], VIEW, PTA, 0)


def key(k):
    e.call("lira_ui_key_get", k)
    return e.uc.reg_read(A.UC_ARM_REG_R0)


def get(c):
    e.call("lira_ui_get", c)
    v = e.uc.reg_read(A.UC_ARM_REG_R0)
    return v - (1 << 32) if v & 0x80000000 else v


PLD = 190                                                      # a plate row d (the plates start at foot_y - 26 = 183)
touch("down", 3 + 38 * 2 + 5, PLD, up=False)
e.call("solo_touch_up", VIEW, PTA, 0)
check("a tap on plate 3 switches the voice on (latch)", key(2) == 1 and key(1) == 0)
touch("down", 3 + 38 * 2 + 5, PLD)
e.call("solo_touch_up", VIEW, PTA, 0)
check("... and the next tap off", key(2) == 0)
touch("down", 314 - 20, 7)
e.call("solo_touch_up", VIEW, PTA, 0)
touch("down", 3 + 38 * 5 + 5, PLD)
check("HOLD: plate 6 is on only while the finger is down", key(5) == 1)
e.call("solo_touch_up", VIEW, PTA, 0)
check("... released: off", key(5) == 0)
touch("down", 314 - 20, 7)
e.call("solo_touch_up", VIEW, PTA, 0)
# pair 2's FM source (top button of its column) and touch (bottom button)
touch("down", 3 + 77 + 10, 95 + 5)
e.call("solo_touch_up", VIEW, PTA, 0)
check("pair 2's FM Source steps (Self -> Off)", get(105) == 1, get(105))
touch("down", 3 + 77 + 10, 95 + 86 - 6)
e.call("solo_touch_up", VIEW, PTA, 0)
check("pair 2's Touch steps (Fast -> Follow)", get(101) == 2, get(101))
# the Hold 1-4 slider (middle block, left): half way along sets about 500; Hold 5-8 is separate
touch("down", 3 + 76, 65 + 6, up=False)
check("Hold 1-4 follows the finger (about half way)", 450 < get(121) < 550 and get(122) == 0, (get(121), get(122)))
touch("move", 3 + 140, 65 + 6)
check("... and drags", get(121) > 850, get(121))
e.call("solo_touch_up", VIEW, PTA, 0)
touch("down", 159 + 76, 65 + 6)
e.call("solo_touch_up", VIEW, PTA, 0)
check("Hold 5-8 is its own slider", 450 < get(122) < 550, get(122))
# a vertical tune fader: pair 1, first fader, bottom = 0, top = 1000 (tune faders span d 133..167)
touch("down", 3 + 5 + 10, 133 + 17)
check("a Tune fader: touch half way up sets about 500", 400 < get(0) < 600, get(0))
e.call("solo_touch_up", VIEW, PTA, 0)
# the encoders: knob 0 turns the first slider of the active group (TUNE 1 now)
MSG = 0x24038100
def knob(n, counts):
    e.uc.mem_write(MSG, struct.pack("<H", 0x32) + bytes(10) + struct.pack("<h", n) + bytes(2) + struct.pack("<h", counts))
    e.call("looper_view_msg", VIEW, MSG)
t0 = get(0)
knob(1, 80)                                                     # raw knob 1 is the bottom-left encoder (knob 0 after the swap)
check("the left-bottom encoder turns Tune 1 (TUNE group)", get(0) == min(1000, t0 + 10), (t0, get(0)))
touch("down", 3 + 10, 95 + 20)                                  # a MOD slider (pair 1): the active group becomes PAIRS
e.call("solo_touch_up", VIEW, PTA, 0)
m0 = get(108)
knob(1, 80)
check("after touching MOD 1 the same encoder turns MOD 1", get(108) == min(1000, m0 + 10) and get(0) == t0 + 10, (m0, get(108)))
f = draw(0)
check("redraw after the touches still inside the screen", all(x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224 for x in f))
e.uc.mem_write(APP + 0x8CA4, b"\x2f")                                # the mixer view is showing
# the pad button on the page goes back to the stock pad screen; MIX goes to the looper's first page
screens.clear()
e.call("solo_pad_pressed", APP, 0, 0, 0)
check("PAD on the LIRA page goes back to the stock pad screen", screens == [2], screens)
print("failures:", fails)
sys.exit(1 if fails else 0)
