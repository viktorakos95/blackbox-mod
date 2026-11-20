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
    e.uc.mem_write(e.sym["looper_full"], b"\x00")      # not full screen: the geometry the checks expect

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


drawn_fills, drawn_pixels = [], []

import math
import os
PAL = [struct.unpack("<I", open("firmware/BLACKBOX-3.1.9.bin", "rb").read()[0x080F1D80 - 0x08040000 + 4 * i:][:4])[0] for i in range(0x24)]


import re
for m_ in re.finditer(r"\{(0x[0-9a-f]{2}), (0x[0-9a-f]{8})\}", open("src/lira_ui.inc").read()):
    PAL[int(m_.group(1), 16)] = int(m_.group(2), 16)               # the page's own colours (src/lira_ui.inc, lu_theme)


GRABS = {}


def render(fills_, pixels_, pal):
    from PIL import Image
    im = Image.new("RGB", (320, 240), (0, 0, 0))
    px = im.load()
    for x, y, w, h, c, _ in fills_:
        col = pal[c] & 0xFFFFFF
        rgb = ((col >> 16) & 255, (col >> 8) & 255, col & 255)
        for yy in range(max(0, 240 - y - h), min(240, 240 - y)):
            for xx in range(max(0, x), min(320, x + w)):
                px[xx, yy] = rgb
    for x, y, c in pixels_:
        col = pal[c] & 0xFFFFFF
        if 0 <= x < 320 and 0 <= 239 - y < 240:
            px[x, 239 - y] = ((col >> 16) & 255, (col >> 8) & 255, col & 255)
    return im


def snap(name):
    """LIRA_PNG=<dir>: write what the last draw() recorded as a picture (320 x 240, origin bottom left, palette at 0x080f1d80)."""
    out = os.environ.get("LIRA_PNG")
    if not out:
        return
    GRABS[name] = (list(drawn_fills), list(drawn_pixels))
    from PIL import Image
    im = Image.new("RGB", (320, 240), (0, 0, 0))
    px = im.load()
    for x, y, w, h, c, _ in drawn_fills:
        col = PAL[c] & 0xFFFFFF
        rgb = ((col >> 16) & 255, (col >> 8) & 255, col & 255)
        for yy in range(max(0, 240 - y - h), min(240, 240 - y)):
            for xx in range(max(0, x), min(320, x + w)):
                px[xx, yy] = rgb
    for x, y, c in drawn_pixels:
        col = PAL[c] & 0xFFFFFF
        if 0 <= x < 320 and 0 <= 239 - y < 240:
            px[x, 239 - y] = ((col >> 16) & 255, (col >> 8) & 255, col & 255)
    os.makedirs(out, exist_ok=True)
    im.resize((640, 480), Image.NEAREST).save(os.path.join(out, name + ".png"))


def draw(cell=0):
    fills.clear()
    pixels.clear()
    celldraw.clear()
    e.call("looper_cell_draw", VIEW + 0x3AC + cell * 0x1A0, CTX, count=50_000_000)
    drawn_fills[:] = [x for x in fills]
    drawn_pixels[:] = list(pixels)
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
e.w32(0x24000088, 0xC4000000)                                   # the allocator's top and floor: 64 MB free
e.w32(0x2400008C, 0xC0000000)
e.call("interp_register", 0x24060000, 0xF1, 0, 0, 2, 0)
PG = 0x38800F00 if False else None
screens = []
e.stub(0x0809EAEC, lambda: screens.append(e.arg(1)))
e.stub(0x080B5E44)
e.uc.mem_write(APP + 0x8CA4, b"\x02")                                # on the stock pad screen
screens.clear()
e.call("solo_song_pressed", APP, 0, 0, 0)
check("SONG pressed on the pad screen opens the mixer view (the page)", screens == [0x2F], screens)
e.call("looper_page_want", 7)
e.call("solo_set_mode", VIEW, 1)
e.call("looper_page_tab")
check("... on tab 7, the LIRA page (TRIG is 6)", e.uc.reg_read(A.UC_ARM_REG_R0) == 7, e.uc.reg_read(A.UC_ARM_REG_R0))
e.call("lira_ui_action", 8)                                           # BTN on (the page starts with BTN off; these tests are about the plates)
f = draw(0)
snap("voices14")
check("LIRA page painted: background first", f and f[0][:4] == (3, 0, 314, 224) and f[0][4] == 0x02, f[:1])
inside = all(x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224 for x in f)
check("all fills lie inside the screen", inside, [x for x in f if not (x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224)][:4])
check("text inside the screen", pixels and all(3 <= px < 317 and 0 <= py < 224 for px, py, _ in pixels), pixels[:3])
check("eight plates (32 px wide bodies) at the bottom", len({x[0] for x in f if x[2] == 32 and x[3] > 20 and x[1] < 60}) >= 8, sorted({(x[0], x[2], x[3]) for x in f if x[1] < 60})[:12])
check("the four encoder knobs have white labels", sum(1 for p_ in pixels if p_[2] == 0x0F) > 40, sum(1 for p_ in pixels if p_[2] == 0x0F))


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


def sel():
    e.call("lira_ui_sel", 0xFFFFFFFF)
    return e.uc.reg_read(A.UC_ARM_REG_R0)


def chord(id):
    e.call("lira_ui_chord_get", id)
    return e.uc.reg_read(A.UC_ARM_REG_R0)


def pitch(v):
    e.call("lira_ui_pitch", v)
    return e.uc.reg_read(A.UC_ARM_REG_R0)


def note(v):
    e.call("lira_ui_note", v)
    r = e.uc.reg_read(A.UC_ARM_REG_R0)
    return r - (1 << 32) if r & 0x80000000 else r


MSG = 0x24038100
def knob(n, counts):                                              # n = the raw knob number of the message
    e.uc.mem_write(MSG, struct.pack("<H", 0x32) + bytes(10) + struct.pack("<h", n) + bytes(2) + struct.pack("<h", counts))
    e.call("looper_view_msg", VIEW, MSG)


def drag(x, d0, d1):
    touch("down", x, d0, up=False)
    touch("move", x, d1)
    e.call("solo_touch_up", VIEW, PTA, 0)


def tap(x, d):
    touch("down", x, d)
    e.call("solo_touch_up", VIEW, PTA, 0)


leds = []
e.stub(0x080436B0, lambda: leds.append((e.arg(0), e.arg(1), e.arg(2))))
PLD = 180                                                      # the plates: d 157 .. 205; the grid: voice v is at x = 5 + 38 v
tap(5 + 38 * 2 + 19, PLD)
check("a tap on plate 3 switches the voice on (latch)", key(2) == 1 and key(1) == 0)
check("the button LEDs follow: LED 3 on, the others off, held out of the stock dimming",
      (0x240021BC, 2, 1) in leds[-8:] and (0x240021BC, 1, 0) in leds[-8:] and e.r8(0x24009A2C + 12 * 2 + 8) == 2, leds[-8:])
tap(5 + 38 * 2 + 19, PLD)
check("... and the next tap off", key(2) == 0)
tap(314 - 20, 7)
touch("down", 5 + 38 * 3 + 19, PLD)
check("HOLD: plate 4 is on only while the finger is down", key(3) == 1)
e.call("solo_touch_up", VIEW, PTA, 0)
check("... released: off", key(3) == 0)
tap(314 - 20, 7)
# VOICES tab, pair 2 = panel 1 (x 81..157): MOD top-left, SHARP top-right, FM source bottom-left, TOUCH bottom-right
def set_ticks(n):                                                   # find looper_state.ticks by probing, then write it
    for off in range(0, 0x300, 4):
        old = e.r32(STATE + off)
        e.w32(STATE + off, 0x12345678)
        e.call("looper_ticks")
        hit = e.uc.reg_read(A.UC_ARM_REG_R0) == 0x12345678
        e.w32(STATE + off, n if hit else old)
        if hit:
            return
    raise SystemExit("ticks not found")


T = [1000]


def slow_tap(x, d):                                                 # a tap well after the last one (two quick taps are a double tap: back to the default)
    T[0] += 300
    set_ticks(T[0])
    tap(x, d)


slow_tap(5 + 76 + 19, 85)
check("pair 2's FM Source button steps (Self -> Off)", get(105) == 1, get(105))
for _ in range(7):
    slow_tap(5 + 76 + 19, 85)
check("... eight taps in it reaches OUT (8), the ninth wraps to SELF", get(105) == 8, get(105))
slow_tap(5 + 76 + 19, 85)
check("... wrapped", get(105) == 0, get(105))
slow_tap(5 + 76 + 19, 85)
slow_tap(5 + 76 + 19, 85)
tap(5 + 76 + 19, 85)
check("B: a fast double tap on the FM button goes back to SELF (0)", get(105) == 0, get(105))
slow_tap(5 + 76 + 19, 85)

tap(5 + 76 + 38 + 19, 85)
check("pair 2's Touch button steps (Fast -> Follow)", get(101) == 2, get(101))
drag(5 + 76 + 19, 60, 20)
check("pair 2's MOD knob: dragging up 40 px raises it by 500", 480 < get(109) < 520, get(109))
drag(5 + 38 + 19, 60, 20)
check("pair 1's SHARP knob raised by 500, its MOD untouched", abs(get(130) - 957) < 8 and get(108) == 0, (get(130), get(108)))
check("every voice starts at the same tune default", len({get(v) for v in range(8)}) == 1 and get(0) == 500, [get(v) for v in range(8)])
drag(5 + 38 * 7 + 19, 140, 120)
check("voice 8's tune knob (the strip): drag up 20 px = +250", 740 < get(7) < 760, get(7))
drag(5 + 19, 140, 120)
check("voice 1's tune knob: +250", 740 < get(0) < 760, get(0))
tap(5 + 19, 140)
check("a plain tap on a knob changes nothing", 740 < get(0) < 760, get(0))
w0 = get(2)
drag(5 + 38 * 2 + 19, 140, 138)
check("a 2 px wiggle on a knob changes nothing (dead zone)", get(2) == w0, (w0, get(2)))
# the four encoders: voices 1 2 3 4 are the top left, top right, bottom left and bottom right ones (here with the top bar on FREE: a unit a click)
e.call("lira_ui_action", 6)
e.call("lira_ui_sel", 0)
ALL = list(range(8)) + [170 + v for v in range(8)]
for raw, cid, sgn in ((0, 170, -1), (2, 171, -1), (1, 0, 1), (3, 1, 1)):    # the first pair's square: level 1 top left, level 2 top right, voice 1 bottom left, voice 2 bottom right
    before = [get(v) for v in ALL]
    knob(raw, 80 * sgn)
    after = [get(v) for v in ALL]
    k = ALL.index(cid)
    check("raw knob %d turns control %d only (the first square: 2 levels over 2 voices)" % (raw, cid), after[k] == before[k] + 10 * sgn and
          all(after[v] == before[v] for v in range(16) if v != k), (before, after))
# BACK moves the encoders to voices 5-8
e.stub(0x080A2E60, lambda: None)
e.uc.mem_write(APP + 0x8CA4, b"\x2f")
def button(idx, id=0xF9):
    e.uc.mem_write(MSG, struct.pack("<H", id) + bytes(10) + struct.pack("<I", idx))
    e.call("looper_app_msg", APP, MSG)
def set_ticks(n):                                                   # find looper_state.ticks by probing, then write it
    for off in range(0, 0x300, 4):
        old = e.r32(STATE + off)
        e.w32(STATE + off, 0x12345678)
        e.call("looper_ticks")
        hit = e.uc.reg_read(A.UC_ARM_REG_R0) == 0x12345678
        e.w32(STATE + off, n if hit else old)
        if hit:
            return
    raise SystemExit("ticks not found")


for cid, v in ((170, 1000), (171, 1000), (0, 500), (1, 500)):
    e.call("lira_ui_set", cid, v)
e.call("lira_ui_sel", 0)
set_ticks(5000)
check("the encoder group is the first voice pair (bank 0)", sel() == 0, sel())
button(11, 7)
e.call("looper_page_tab")
check("BACK on the page goes back to the TRIG page", e.uc.reg_read(A.UC_ARM_REG_R0) == 6, e.uc.reg_read(A.UC_ARM_REG_R0))
e.call("looper_page_goto", VIEW, 7)
set_ticks(5200)
button(0, 0xc)
check("INFO: the next pair, 3-4 (bank 1)", sel() == 16, sel())
before = [get(v) for v in ALL]
knob(0, -80)
check("... the top-left encoder now turns level 3", get(172) == before[ALL.index(172)] - 10 and get(170) == before[ALL.index(170)], (before, [get(v) for v in ALL]))
e.call("lira_ui_set", 172, 1000)
e.call("lira_ui_action", 6)                                         # back to STEP: outside chord mode a click is a semitone, as in chord mode
set_ticks(5100)
e.call("lira_ui_sel", 0)                                            # voices 1-4 (BACK leaves the page here; their branch flipped the bank with it)
e.call("lira_ui_tunepitch", 0)
h0 = e.uc.reg_read(A.UC_ARM_REG_R0)
knob(1, 100)
e.call("lira_ui_tunepitch", 0)
h1 = e.uc.reg_read(A.UC_ARM_REG_R0)
check("STEP: an encoder click moves the voice to the next real note (%d -> %d)" % (h0, h1), h1 % 100 == 0 and 0 < h1 - h0 <= 100, str((h0, h1)))
knob(1, 100)
e.call("lira_ui_tunepitch", 0)
h2 = e.uc.reg_read(A.UC_ARM_REG_R0)
check("... and the next one (%d)" % h2, h2 == h1 + 100, str((h1, h2)))
knob(1, -100)
e.call("lira_ui_tunepitch", 0)
check("... and a click the other way goes back down", e.uc.reg_read(A.UC_ARM_REG_R0) == h1, str(e.uc.reg_read(A.UC_ARM_REG_R0)))
knob(1, 40)
e.call("lira_ui_tunepitch", 0)
check("... less than a click does nothing yet", e.uc.reg_read(A.UC_ARM_REG_R0) == h1, str(e.uc.reg_read(A.UC_ARM_REG_R0)))
knob(1, 60)
set_ticks(5300)
e.call("lira_ui_sel", 16)                                           # voices 5-8
f = draw(0)
snap("voices58")
check("VOICES tab, bank 1, drawn inside the screen", all(x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224 for x in f))
# a panel is the encoder group after a touch on it: pair 3 (panel 2): the encoders turn its MOD, SHARP, FM source, TOUCH
tap(5 + 152 + 19, 60)
check("a touch on a pair panel makes it the encoder group", sel() == 4 << 4, sel())
m0, s0 = get(110), get(132)
knob(0, 80)                                                       # raw 0 = top-left = MOD
knob(2, 80)                                                       # raw 2 = top-right = SHARP
check("top-left / top-right encoders turn MOD / SHARP of the panel", get(110) == m0 + 10 and get(132) == s0 + 10, (m0, get(110), s0, get(132)))
src0 = get(106)
knob(1, 300)                                                      # raw 1 = bottom-left = FM source (a step per ~300 counts)
check("the bottom-left encoder steps the FM source", get(106) == (src0 + 1) & 7, (src0, get(106)))
# the SOUND tab
tap(5 + 52 + 10, 215)
check("the SOUND tab (first panel is the encoder group)", sel() == 1 | 2 << 4, sel())
t0 = get(16)
drag(5 + 19, 35, 75)
check("TIME 1 (top-left of DELAY): dragging down 40 px lowers it by 500", abs(get(16) - (t0 - 500)) <= 3, (t0, get(16)))
knob(0, 80)
check("the top-left encoder turns TIME 1", 8 <= get(16) - (t0 - 500) <= 12, (t0, get(16)))
knob(3, 80)
check("the bottom-right encoder turns MIX", get(19) != 0, get(19))
f = draw(0)
snap("sound")
check("SOUND drawn inside the screen", all(x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224 for x in f))
tap(5 + 152 + 19, 85)
check("SYNC A is an on / off button: a tap turns it on at 1/4 (6)", chord(144) == 6, chord(144))
knob(1, 300)                                                      # raw 1 = bottom-left = the cell's encoder (a step per ~300 counts)
check("... its own encoder still steps the division (1/4 -> 1/8D)", chord(144) == 7, chord(144))
hz0 = get(14)
knob(0, 300)                                                      # the LFO A encoder (top-left of the panel) while sync is on: a click per division
check("synced: the LFO A encoder steps the DIVISION (1/8D -> 1/8) and leaves the Hz alone", chord(144) == 8 and get(14) == hz0, (chord(144), get(14), hz0))
knob(0, -300)
knob(0, -300)
check("... and back down (1/8 -> 1/4D -> 1/4) , no wrap", chord(144) == 6, chord(144))
for _ in range(12):
    knob(0, -300)
check("... it stops at the first division (4 BAR), never off by turning", chord(144) == 1, chord(144))
for _ in range(15):
    knob(0, 300)
check("... and at the last (1/16T)", chord(144) == 11 and get(14) == hz0, (chord(144), get(14)))
tap(5 + 152 + 19, 85)
check("the sync button off: sync 0", chord(144) == 0, chord(144))
knob(0, 80)
check("off: the LFO A encoder edits the Hz again", get(14) != hz0 and chord(144) == 0, (get(14), hz0))
slow_tap(5 + 152 + 19, 85)
check("on again: back at the division it had (1/16T)", chord(144) == 11, chord(144))
drag(5 + 152 + 19, 35, 75)
check("synced: dragging the LFO A knob down picks a lower division, Hz untouched", chord(144) < 11 and get(14) != 0, chord(144))
slow_tap(5 + 152 + 19, 85)
slow_tap(5 + 76 + 19, 85)
check("DLY MOD + SYNC: SYNC 1 (bottom left) is an on / off button too (1/4)", chord(146) == 6, chord(146))
t1 = get(16)
tap(5 + 19, 40)                                                   # the TIME 1 knob selects the DELAY encoders
knob(0, 300)
check("synced: the TIME 1 encoder steps the division (1/8D), the ms stay", chord(146) == 7 and get(16) == t1, (chord(146), get(16), t1))
tap(5 + 76 + 19, 85)
slow_tap(5 + 76 + 38 + 19, 85)
check("... SYNC 2 (bottom right) switches the second line on", chord(147) == 6, chord(147))
slow_tap(5 + 76 + 38 + 19, 85)
check("... and off", chord(147) == 0, chord(147))
# the CHORD tab
# a level knob per voice, above the tune knobs on every page
check("every voice level starts at full", all(get(170 + v) == 1000 for v in range(8)), [get(170 + v) for v in range(8)])
drag(5 + 38 + 19, 118, 150)
check("LEVEL 2 (the small knob above voice 2's tune knob): dragging down 32 px lowers it by 400", 590 < get(171) < 610 and get(170) == 1000, (get(170), get(171)))
check("... and it is the encoders' group (the first pair's square, bank 0)", sel() >> 4 == 0, sel())
knob(0, -80)
check("the top-left encoder turns LEVEL 1 now", get(170) == 990 and get(171) < 610, (get(170), get(171)))
tap(5 + 38 + 19, 118)
tap(5 + 38 + 19, 118)
check("a double tap puts the level back to full", get(171) == 1000, get(171))
tap(5 + 156 + 10, 215)                                      # MIX: the last tab (CHORD before it)
check("the MIX tab: the output stage in one row", sel() == 2 | 2 << 4, sel())
d0 = get(22)
drag(5 + 19, 60, 20)
check("DRIVE (the first knob of the row): dragging up raises it", get(22) > d0, (d0, get(22)))
p0 = get(123)
drag(5 + 76 + 19, 60, 20)
check("the PITCH panel (second panel of MIX): the first knob is P 1-4, dragging raises it", get(123) > p0, (p0, get(123)))
tap(5 + 152 + 19, 85)                                             # the GESTURE panel: LEN (bottom-left of the third panel is UNDO, top-right LEN)
tap(5 + 152 + 38 + 19, 47)
check("the GESTURE panel: LEN steps 1 -> 2 bars", chord(148) == 1, chord(148))
tap(160, 7)
check("the REC switch in the top bar arms the recorder", chord(158) == 1, chord(158))
tap(160, 7)
check("... and a second press cancels it", chord(158) == 0, chord(158))
check("PAN starts closed (0: mono), DRIFT at 400, VIB on", get(180) == 0 and get(181) == 400 and chord(195) == 1, (get(180), get(181), chord(195)))
drag(5 + 228 + 19, 60, 20)
check("the FEEL panel: dragging PAN up 40 px opens it by 500", 490 < get(180) < 510 and sel() == 2 | 5 << 4, (get(180), hex(sel())))
knob(0, 80)
check("... and the top-left encoder of the panel turns it", get(180) == 510, get(180))
tap(5 + 228 + 19, 47)
tap(5 + 228 + 19, 47)
check("a double tap puts PAN back to 0", get(180) == 0, get(180))
drag(5 + 228 + 38 + 19, 60, 20)
check("DRIFT (top right) drags: 400 + 500", get(181) >= 900, get(181))
tap(5 + 228 + 38 + 19, 47)
tap(5 + 228 + 38 + 19, 47)
check("a double tap puts DRIFT back to 400", get(181) == 400, get(181))
tap(5 + 228 + 38 + 19, 85)
check("the ENGINE button (bottom right of the panel) switches the engine off", chord(194) == 1, chord(194))
tap(5 + 228 + 38 + 19, 85)
check("... and on again", chord(194) == 0, chord(194))
tap(5 + 76 + 19, 85)
check("the VIB button (PITCH panel, bottom left) switches the vibrato off", chord(195) == 0, chord(195))
tap(5 + 76 + 19, 85)
check("... and on again", chord(195) == 1, chord(195))
e.call("lira_ui_set", 2, 523)                                    # voice 3 between notes
e.call("lira_ui_tunepitch", 2)
off_note = e.uc.reg_read(A.UC_ARM_REG_R0) % 100
tap(5 + 104 + 10, 215)                                           # the CHORD tab (third in the footer): QUANT lives in its VOICING panel now
check("QUANT sits on the CHORD tab, VOICING panel, bottom left (TUNE and SHUFFLE changed places in n14)", sel() == 3 | 2 << 4, sel())
tap(5 + 76 + 19, 85)
e.call("lira_ui_tunepitch", 2)
snapped = e.uc.reg_read(A.UC_ARM_REG_R0)
check("QUANT (CHORD tab, VOICING panel, bottom left) is a one-shot: voice 3 was %d cents off a note, now %d (a note)" % (off_note, snapped % 100), snapped % 100 == 0, str(snapped))
e.call("lira_ui_set", 2, get(2) + 3)
e.call("lira_ui_tunepitch", 2)
check("... (turning the knob a little leaves the note)", e.uc.reg_read(A.UC_ARM_REG_R0) % 100 != 0, str(e.uc.reg_read(A.UC_ARM_REG_R0)))
tap(5 + 156 + 10, 215)                                           # back to the MIX tab
check("... and the MIX tab is back", sel() & 15 == 2, sel())
f = draw(0)
snap("mix")
check("MIX drawn inside the screen", all(x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224 for x in f))
# the LFO panel of SOUND: SHAPE, COMBINE, LINK, DLY SRC
tap(5 + 52 + 10, 215)
tap(5 + 228 + 19, 85)
tap(5 + 228 + 38 + 19, 85)
check("the LFO panel: LINK switches on, DLY SRC goes LFO -> self", chord(162) == 1 and chord(163) == 0, (chord(160), chord(161), chord(162), chord(163)))
tap(5 + 228 + 19, 47)
tap(5 + 228 + 38 + 19, 47)
check("SHAPE and COMBINE toggle", chord(160) == 1 and chord(161) == 1, (chord(160), chord(161)))
tap(5 + 104 + 10, 215)                                      # CHORD: the third tab
check("the CHORD tab", sel() == 3 | 2 << 4, sel())
check("chord mode starts off, MAJ 9, root C", chord(150) == 0 and chord(140) == 0 and chord(141) == 0, (chord(150), chord(140), chord(141)))
knob(0, 300)
check("the top-left encoder steps the tonal space", chord(140) == 1, chord(140))
knob(2, 300)
check("the top-right encoder steps the root", chord(141) == 1, chord(141))
knob(3, 300)
check("the bottom-right encoder steps the span", chord(143) == 3, chord(143))
tap(5 + 76 + 19, 47)
check("the CHORD button switches chord mode on", chord(150) == 1)
n0 = [note(v) for v in range(8)]
check("eight notes in 33..93, all pitch classes of MAJ 7#11 on D-flat... (in the space)", all(33 <= x <= 93 for x in n0) and len(set(n0)) == 8, n0)
drag(5 + 19, 140, 120)
check("in chord mode a drag on a voice knob moves its note (15 semitones for 20 px)", note(0) == min(93, n0[0] + 15) and [note(v) for v in range(1, 8)] == n0[1:], (n0, [note(v) for v in range(8)]))
knob(1, 100)
check("... and an encoder turn moves it a semitone", note(0) == min(93, min(93, n0[0] + 15) + 1), note(0))
knob(1, -100)
n0 = [note(v) for v in range(8)]
check("STEP mode shows whole notes (pitch is an exact note)", pitch(0) % 100 == 0, pitch(0))
tap(180, 7)
check("the top-bar STEP / FREE switch selects FREE", chord(157) == 1)
knob(1, 80)
check("FREE: an encoder click moves the voice off the note (0.05 semitone a unit, 50 cents for 80 counts, as before n8)", pitch(0) % 100 != 0 and abs(pitch(0) - n0[0] * 100) == 50, pitch(0))
knob(1, -80)
check("... and back on the note it is exact again", pitch(0) == n0[0] * 100, pitch(0))
# 2 (n14): the free chord pitch: every value reachable, a short flat spot at each exact note (no catch, no hold), the name only at exactly 0 cents
def stall(r):
    e.call("lira_ui_pitch_stall", r)
    v = e.uc.reg_read(A.UC_ARM_REG_R0)
    return v - (1 << 32) if v & 0x80000000 else v
def unstall(h):
    e.call("lira_ui_pitch_unstall", h)
    return e.uc.reg_read(A.UC_ARM_REG_R0)
sweep = [stall(5280 + i) for i in range(2640 + 1)]                  # the finger moving up one raw cent at a time, two octaves (raw 5280 = 48.00)
vals = sorted(set(sweep))
check("a slow sweep over two octaves produces EVERY integer cent value in range (nothing is hidden or skipped)", vals == list(range(vals[0], vals[-1] + 1)), (vals[0], vals[-1], len(vals)))
runs, cur_run, prev = [], 0, None
for h in sweep:
    if h == prev:
        cur_run += 1
    else:
        if prev is not None and prev % 100 == 0 and cur_run:
            runs.append(cur_run)
        cur_run = 1
    prev = h
check("each exact note holds for a flat run of at most 11 raw cents (10 cents of finger travel); every other value just once", max(runs) <= 11 and min(runs) >= 10, (min(runs), max(runs)))
coarse = [stall(5280 + 5 * i) for i in range(481)]                  # a 5-cent step: the flat run is at most 3 steps
cr, c0, pv = [], 0, None
for h in coarse:
    if h == pv:
        c0 += 1
    else:
        if pv is not None and pv % 100 == 0 and c0:
            cr.append(c0)
        c0 = 1
    pv = h
check("with 5-cent steps the flat run at a note is at most 3 steps", max(cr) <= 3, max(cr))
notes_met = sorted({h for h in sweep if h % 100 == 0})
check("the notes keep exactly 100 cents between them (4800 .. 7200: 25 notes)", notes_met == list(range(4800, 7300, 100)), notes_met[:4])
check("the finger travel per note is 110 cents (10 cents more than before: a few hundredths of a pixel at 75 cents a pixel)", unstall(5000) - unstall(4900) == 110, (unstall(5000), unstall(4900)))
check("unstall and stall are inverses off the note (every cent)", all(stall(unstall(h)) == h for h in range(4801, 7200, 7)))
# the drag as the touch handler does it: dy pixels at 75 cents each from a note
e.call("lira_ui_pitch_set", 0, 6000)
# the encoder: 5-cent clicks from a note: every value comes by, the note is held for 2-3 clicks
e.call("lira_ui_pitch_set", 0, 5900)
seq = []
for _ in range(60):
    e.call("lira_ui_pitch_enc", 0, 5)
    e.call("lira_ui_pitch", 0)
    seq.append(e.uc.reg_read(A.UC_ARM_REG_R0))
check("FREE encoder, 5-cent clicks up through the note 6000: every click moves except 2-3 on the note itself", 6000 in seq and seq.count(6000) <= 3 and seq[-1] > 6000 and all(b >= a for a, b in zip(seq, seq[1:])), (seq.count(6000), seq[:4], seq[-1]))
seqd = []
for _ in range(60):
    e.call("lira_ui_pitch_enc", 0, -5)
    e.call("lira_ui_pitch", 0)
    seqd.append(e.uc.reg_read(A.UC_ARM_REG_R0))
check("... and back down the same way (it passes the note and goes on below it)", seqd.count(6000) <= 3 and seqd[-1] < 6000, (seqd.count(6000), seqd[-1]))
def penc_run(start, step, n):
    e.call("lira_ui_pitch_set", 0, start)
    out = []
    for _ in range(n):
        e.call("lira_ui_pitch_enc", 0, step)
        e.call("lira_ui_pitch", 0)
        out.append(e.uc.reg_read(A.UC_ARM_REG_R0))
    return out
for step in (1, 5, 40, 95, 250, -1, -5, -40, -95, -250):
    up = step > 0
    a0, a1 = (4800, 7200) if up else (7200, 4800)
    q = penc_run(a0 + (1 if up else -1), step, 2400 // abs(step) + 24 * (3 + 100 // abs(step)) + 40)
    q = [h for h in q if min(a0, a1) <= h <= max(a0, a1)]
    ok = all(((n * 100) in q) for n in range(49, 72)) and all(q.count(n * 100) == 3 for n in range(49, 72)) and all((b - a) * (1 if up else -1) >= 0 for a, b in zip(q, q[1:]))
    check(f"FREE encoder, {step:+d} per click across two octaves: every note is caught (3 stops: arrival + 2 absorbed), never skipped, no step backwards", ok, (step, [n for n in range(49, 72) if q.count(n * 100) != 3][:5]))
    if abs(step) < 100:
        check(f"... and every cent is reachable on the way ({step:+d})", len(set(q)) >= (2400 * 0.9) // max(abs(step), 1), len(set(q)))
r = penc_run(5900, 5, 3) + penc_run(6000, 5, 1)
e.call("lira_ui_pitch_set", 0, 5995); e.call("lira_ui_pitch_enc", 0, 5)
x = [pitch(0)]; e.call("lira_ui_pitch_enc", 0, -5); x.append(pitch(0))
check("reversing on the note releases at once (6000, then 5995)", x == [6000, 5995], x)
e.call("lira_ui_pitch_set", 0, 6000); e.call("lira_ui_pitch_enc", 0, 5)
drag(5 + 19, 140, 139)
check("a drag moves it freely (0.75 semitone a pixel) ...", pitch(0) != n0[0] * 100, pitch(0))
e.call("lira_ui_pitch_set", 1, 6000 + 20)
drag(5 + 38 + 19, 140, 139)
check("... and no catch: a pixel that lands 5 cents from a note shows exactly that (6095), every value reachable", pitch(1) == 6095, pitch(1))
# the knob outside chord mode: the 12-cent catch as before n10 (no hold), the name only on an exact note
was_chord, was_free = chord(150), chord(157)
if chord(150):
    e.call("lira_ui_action", 0)                                      # chord mode off: the knobs tune freely
if not chord(157):
    e.call("lira_ui_action", 6)                                      # FREE
def tuneh(v=0):
    e.call("lira_ui_tunepitch", v)
    return e.uc.reg_read(A.UC_ARM_REG_R0)
def find(lo, hi, side):
    for val in range(300, 700):
        e.call("lira_ui_set", 0, val)
        h = tuneh()
        d = h - ((h + 50) // 100) * 100
        if lo <= side * d <= hi:
            return val
    return None
assert find(8, 12, 1) is not None
e.call("lira_ui_tune_detent", 0)
check("the knob's detent catches within 12 cents of a note (as before)", tuneh() % 100 == 0, tuneh())
assert find(20, 30, 1) is not None
e.call("lira_ui_tune_detent", 0)
check("... and nothing further away is caught or held (20..30 cents stays as it is)", tuneh() % 100 != 0, tuneh())
if was_chord:
    e.call("lira_ui_action", 0)
if chord(157) != was_free:
    e.call("lira_ui_action", 6)
tap(180, 7)
f = draw(0)
snap("chord")
check("CHORD drawn inside the screen", all(x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224 for x in f))
# the ENV tab (fifth): FAST / SLOW envelope times, VELOCITY, MIDI to come
tap(5 + 208 + 10, 215)
check("the ENV tab (fifth in the footer)", sel() == 4 | 2 << 4, sel())
f = draw(0)
snap("env")
check("ENV drawn inside the screen", all(x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224 for x in f))
check("ENV defaults: FAST attack a snap, releases as built, slow attack 0.6 s, velocity off", [get(c) for c in range(210, 217)] == [81, 500, 607, 500, 700, 0, 0], [get(c) for c in range(210, 217)])
# ---- SAVE TO PROJECT in the footer (n13): the end of the footer, right of the five tabs
saves = []
e.stub(e.sym["lira_ui_save"] & ~1, lambda: saves.append(e.arg(0)))
tap(5 + 52 * 5 + 20, 215)
check("DRONE-8: a tap on the footer's SAVE button asks for a save (and changes no tab)", saves.count(0) == 1 and sel() & 15 == 4, (saves, hex(sel())))
tap(5 + 52 * 4 + 20, 215)
check("... the ENV tab, just left of it, is still a tab", sel() & 15 == 4 and saves.count(0) == 1, (saves, hex(sel())))
tap(5 + 52 * 1 + 20, 215)
check("... and the SOUND tab next to VOICES still works with the narrower tabs", sel() & 15 == 1 and saves.count(0) == 1, (saves, hex(sel())))
f = draw(0)
check("the footer with the SAVE button is inside the screen", all(x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224 for x in f))
e.stubs.pop(e.sym["lira_ui_save"] & ~1, None)
tap(5 + 208 + 10, 215)
# ---- MIDI learn (the MIDI panel of the ENV tab; the render's message poll is hooked by lira_midi_get)
def learn(op, arg=0):
    e.call("lira_ui_learn", op, arg)
    v = e.uc.reg_read(A.UC_ARM_REG_R0)
    return v - (1 << 32) if v & 0x80000000 else v


def midi(st, cc, val):
    e.call("lira_midi_msg", st, cc, val)


check("MIDI learn starts off, nothing mapped", learn(0) == 0)
slow_tap(233 + 19, 28 + 19)                                       # LEARN (top-left of the MIDI panel)
check("LEARN switches learn mode on", learn(0) & 1 == 1, learn(0))
f = draw(0)
check("learn mode: drawn inside the screen", all(x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224 for x in f))
slow_tap(5 + 19, 47)                                              # FAST ATTACK (top-left of the first panel)
check("in learn mode a tap on a knob chooses it (and does not drag it)", learn(6) == 210 and get(210) == 81, (learn(6), get(210)))
midi(0xB0, 74, 64)
check("the first controller moved is bound to it: CC 74 on channel 1 -> control 210; the choice is used up", learn(5, 210) == 75 and learn(6) == 0xFFFF and learn(0) >> 8 == 1, (learn(5, 210), learn(6), learn(0)))
midi(0xB0, 74, 127)
check("in learn mode a controller moves nothing", get(210) == 81, get(210))
knob(2, 80)                                                       # an encoder turn in learn mode chooses its control too (the top-right one of the panel: FAST RELEASE, 211)
midi(0xB0, 20, 1)
check("... an encoder turn chooses a control as well (and the CC is a second binding)", learn(0) >> 8 == 2, learn(0))
slow_tap(233 + 19, 28 + 19)
check("LEARN again: learn mode off", learn(0) & 1 == 0)
midi(0xB0, 74, 127)
check("a bound controller sets its control: CC 74 at 127 -> 1000", get(210) == 1000, get(210))
midi(0xB0, 74, 0)
check("... and at 0 -> 0", get(210) == 0, get(210))
midi(0xB1, 74, 127)
check("another channel's CC 74 does nothing", get(210) == 0, get(210))
midi(0xE0, 74, 127)
midi(0x90, 74, 127)
check("notes and pitch bend never move a control", get(210) == 0)
midi(0xB0, 74, 64)
check("a middle value maps to about half (64 -> 504)", 500 <= get(210) <= 510, get(210))
slow_tap(5 + 19, 47)
slow_tap(233 + 38 + 19, 28 + 19)                                  # FORGET: the control last touched
check("FORGET removes the mapping of the control last touched", learn(5, 210) == 0, learn(5, 210))
learn(1)
learn(2, 211)
midi(0xB0, 9, 10)
learn(1)
e.call("lira_ui_chord_set", 144, 6)                                   # a learned CC on the LFO A knob: it picks the division while synced, the Hz when not
hz = get(14)
learn(1)
learn(2, 14)
midi(0xB0, 31, 5)                                                   # (the first message binds)
learn(1)                                                          # learn off
midi(0xB0, 31, 127)
check("MIDI learn on the LFO A knob, synced: CC 127 picks the last division (1/16T), Hz unchanged", chord(144) == 11 and get(14) == hz, (chord(144), get(14), hz))
midi(0xB0, 31, 0)
check("... CC 0 the first (4 BAR)", chord(144) == 1 and get(14) == hz, chord(144))
e.call("lira_ui_chord_set", 144, 0)
midi(0xB0, 31, 127)
check("... sync off: the same CC sets the Hz (full)", chord(144) == 0 and get(14) == 1000, (chord(144), get(14)))
slow_tap(233 + 19, 66 + 19)                                       # CLEAR (bottom-left: the third cell)
check("CLEAR removes them all", learn(0) >> 8 == 0, learn(0))
# the stock poll hook: the message is looked at, then goes on unchanged
e.stub(0x0804495E, lambda: (e.uc.mem_write(e.arg(1), bytes([0xB0, 5, 100, 0, 3])), e.uc.reg_write(A.UC_ARM_REG_R0, 1)))
learn(1)
learn(2, 212)
e.call("lira_midi_get", 0x24002E88, 0x24038200, 0x24038210)
check("lira_midi_get returns the stock result and the message, and learns from it (CC 5 -> 212)", e.uc.reg_read(A.UC_ARM_REG_R0) == 1 and learn(5, 212) == 6 and list(e.uc.mem_read(0x24038200, 3)) == [0xB0, 5, 100], learn(5, 212))
learn(1)
learn(4)
# 13: a voice's pitch knob follows a controller, in chord mode (the note) and outside it (the knob, with its detent)
def sigv():
    e.call("lira_ui_sig")
    return e.uc.reg_read(A.UC_ARM_REG_R0)
def pitchv(v):
    e.call("lira_ui_tunepitch", v)
    return e.uc.reg_read(A.UC_ARM_REG_R0)
def bind(id_, cc):
    learn(1); learn(2, id_); midi(0xB0, cc, 1); learn(1)
orig_chord = chord(150)
for chord_state in (1, 0):
    while chord(150) != chord_state:
        e.call("lira_ui_action", 0)
    bind(2, 11)
    if chord_state:
        before = e.call("lira_ui_pitch", 2) or e.uc.reg_read(A.UC_ARM_REG_R0)
        e.call("lira_ui_pitch", 2); p_lo = None
        midi(0xB0, 11, 0)
        e.call("lira_ui_pitch", 2); lo_ = e.uc.reg_read(A.UC_ARM_REG_R0)
        midi(0xB0, 11, 127)
        e.call("lira_ui_pitch", 2); hi_ = e.uc.reg_read(A.UC_ARM_REG_R0)
        check("MIDI learn on a voice's pitch knob in chord mode: CC 0 -> the lowest note, 127 -> the highest (STEP: A1..A6, whole notes)", lo_ == 3300 and hi_ == 9300, (lo_, hi_))
    else:
        midi(0xB0, 11, 0)
        lo_ = get(2)
        midi(0xB0, 11, 127)
        hi_ = get(2)
        check("MIDI learn on a voice's pitch knob outside chord mode: the knob goes 0 -> 1000 and the pitch follows", lo_ == 0 and hi_ == 1000 and pitchv(2) > 0, (lo_, hi_, pitchv(2)))
    learn(4)
while chord(150) != orig_chord:
    e.call("lira_ui_action", 0)
s0 = sigv()
bind(210, 12)
s1 = sigv()
midi(0xB0, 12, 30)
s2 = sigv()
midi(0xB0, 12, 31)
s3 = sigv()
check("14: the page's signature changes with every value MIDI sets (so the knob redraws smoothly, not in coarse steps)", len({s1, s2, s3}) == 3, (s1, s2, s3))
learn(4)
# 1: MIDI learn on buttons: plates, cycling buttons, the top bar's switches, stepped knobs. A button presses when the controller crosses 64 upward.
def momentary():
    e.call("lira_ui_momentary", 0)
    return e.uc.reg_read(A.UC_ARM_REG_R0)
learn(4)
learn(1)
tap(5 + 38 * 2 + 19, PLD)                                          # a plate, in learn mode: chosen, not played
check("learn mode: a tap on a plate chooses it (300 + voice) and does not play it", learn(6) == 302 and key(2) == 0, (learn(6), key(2)))
midi(0xB0, 50, 100)
learn(1)
check("... the first controller moved is bound to the plate", learn(5, 302) == 51, learn(5, 302))
midi(0xB0, 50, 100)
check("a plate (LATCH): the controller going up through 64 switches it on", key(2) == 1, key(2))
midi(0xB0, 50, 110)
check("... staying high does nothing more", key(2) == 1)
midi(0xB0, 50, 5)
check("... and going down does nothing (a latched plate toggles on the way up only)", key(2) == 1)
midi(0xB0, 50, 127)
check("... the next way up switches it off", key(2) == 0, key(2))
learn(1)
tap(294, 7)                                                         # the top bar's LATCH switch (k = 0), in learn mode
check("learn mode: a tap on a top-bar switch chooses it (310 + k)", learn(6) == 310, learn(6))
midi(0xB0, 51, 90)
learn(1)
m0 = momentary()
midi(0xB0, 51, 0)
midi(0xB0, 51, 127)
check("the LATCH / HOLD switch follows a controller: one way up flips it", momentary() != m0, (m0, momentary()))
if momentary() != m0:
    e.call("lira_ui_momentary", 1)                                # (put it back)
# a cycling button (FM source of pair 2, id 105) and a stepped knob (KEY, 141)
bind(105, 60)
f0 = get(105)
midi(0xB0, 60, 127)
check("a cycling button: a controller going up steps it on (FM source +1)", get(105) == (f0 + 1) % 9, (f0, get(105)))
midi(0xB0, 60, 0)
midi(0xB0, 60, 127)
check("... and again on the next way up", get(105) == (f0 + 2) % 9, get(105))
bind(141, 61)
midi(0xB0, 61, 127)
check("a stepped knob (KEY): the controller's range over its steps (127 -> 11)", chord(141) == 11, chord(141))
midi(0xB0, 61, 0)
check("... 0 -> 0", chord(141) == 0, chord(141))
# FORGET on a button (the plate's mapping): the last control chosen
learn(1)
tap(5 + 38 * 2 + 19, PLD)
learn(1)
slow_tap(233 + 38 + 19, 28 + 19) if sel() & 15 == 4 else None
learn(3, 302)
check("FORGET / unlearn removes a button's mapping too", learn(5, 302) == 0, learn(5, 302))
learn(4)
# 15 / 7 (n12): the USB host path. The stock host class driver hands each received buffer to the callback whose copy (0x080446da) lira_usb_rx wraps
stock_copies = []
e.stub(0x080446DA, lambda: stock_copies.append(e.arg(2)))
e.stub(0x0804495E, lambda: e.uc.reg_write(A.UC_ARM_REG_R0, 0))
UB = 0x24038400
def usb_buffer(packets):
    raw = b"".join(bytes(pk) for pk in packets).ljust(0x200, b"\0")
    e.uc.mem_write(UB, raw)
    e.call("lira_usb_rx", 0x24002E88, UB, 0x200, 0)
def poll():
    e.call("lira_midi_get", 0x24002E88, 0x24038200, 0x24038210)
bind(211, 13)
c0 = learn(9)
usb_buffer([(0x0B, 0xB0, 13, 127), (0x09, 0x90, 60, 100)])
check("USB host: the callback counts the packets (2) and still calls the stock copy with the same buffer", learn(9) == c0 + 2 and stock_copies[-1] == 0x200, (c0, learn(9), stock_copies))
check("... and applies nothing in the interrupt (it only queues the messages)", get(211) != 1000)
poll()
check("USB host MIDI (the callback's ring, drained by the next poll): CC 13 at 127 sets control 211 to 1000", get(211) == 1000, get(211))
check("... the MIDI panel's USB count (U) follows", learn(8) >= 2, learn(8))
usb_buffer([(0x0E, 0xE0, 5, 5), (0x0F, 0xF8, 0, 0)])
poll()
check("... pitch bend and clock from USB are ignored", get(211) == 1000)
# a queue message (flag != 0) is not handled twice once the callback has been seen
e.uc.mem_write(0x24038210, b"\x01")
j0, q0 = learn(7), learn(8)
learn(4)
# notes (n12 item 1): a plate mapped to a note: on = press, off = release
learn(1); learn(2, 303); e.call("lira_midi_msg", 0xB0, 1, 1)           # (a controller first: bound as CC, to prove a note can replace it)
learn(2, 303)
e.call("lira_midi_msg", 0x91, 62, 100)                                  # note 62 on channel 2 while a plate waits: bound as a note
learn(1)
e.call("lira_ui_key", 3, 0)
e.call("lira_midi_msg", 0x91, 62, 100)
check("a plate learned from a NOTE: note on presses it (even with the switch on LATCH)", key(3) == 1, key(3))
e.call("lira_midi_msg", 0x81, 62, 0)
check("... note off lets it go (n15: a latched plate used to stay on: a stuck note)", key(3) == 0, key(3))
e.call("lira_midi_msg", 0x90, 62, 100)
check("... another channel's note on does nothing", key(3) == 0, key(3))
e.call("lira_midi_msg", 0x91, 62, 90)
e.call("lira_midi_msg", 0x91, 62, 0)
check("... a note on with velocity 0 is a note off", key(3) == 0, key(3))
e.call("lira_midi_msg", 0x91, 62, 90)
check("... the next note on presses it again", key(3) == 1, key(3))
e.call("lira_midi_msg", 0x81, 62, 0)
# HOLD: note on = down, note off = up
if not momentary():
    e.call("lira_ui_momentary", 1)
e.call("lira_midi_msg", 0x91, 62, 90)
check("HOLD plate on a note: note on = down", key(3) == 1, key(3))
e.call("lira_midi_msg", 0x81, 62, 0)
check("... note off = up", key(3) == 0, key(3))
e.call("lira_ui_momentary", 1)                                          # (back to LATCH)
# a note for a cycling button: steps on note on only
learn(4)
learn(1); learn(2, 105); e.call("lira_midi_msg", 0x90, 40, 100); learn(1)
f0 = get(105)
e.call("lira_midi_msg", 0x90, 40, 100)
e.call("lira_midi_msg", 0x80, 40, 0)
check("a cycling button on a note: note on steps it, note off does not", get(105) == (f0 + 1) % 9, (f0, get(105)))
# a knob does not take a note
learn(4)
learn(1); learn(2, 210); e.call("lira_midi_msg", 0x90, 41, 100)
check("a knob cannot be learned from a note (it keeps waiting for a controller)", learn(5, 210) == 0 and learn(6) == 210, (learn(5, 210), learn(6)))
learn(1)
# 3 (n14 / n15): the VELOCITY panel: SOURCE STEADY (default) / TOUCH / SLIDE / MIDI, AMT, EFFECT SOUND / VOLUME / BOTH
def eff(k):
    e.call("lira_ui_vel_eff", k)
    return e.uc.reg_read(A.UC_ARM_REG_R0)
learn(4)
check("SOURCE starts on STEADY (0), EFFECT on SOUND (0)", get(215) == 0 and get(216) == 0, (get(215), get(216)))
e.call("lira_ui_set", 214, 1000)                                          # AMT full: the effect is the velocity itself
learn(1); learn(2, 304); e.call("lira_midi_msg", 0x90, 70, 100); learn(1)
e.call("lira_ui_tapvel", 4, 0)
e.call("lira_midi_msg", 0x90, 70, 20)
check("STEADY: no velocity effect at all: a soft MIDI note and a touch at the very bottom both play full", eff(4) == 1000, eff(4))
e.call("lira_midi_msg", 0x80, 70, 0)
e.call("lira_ui_set", 215, 1)                                             # TOUCH
e.call("lira_ui_tapvel", 4, 1000)
check("TOUCH: the top of the plate is full", eff(4) == 1000, eff(4))
e.call("lira_ui_tapvel", 4, 250)
check("... a quarter of the way up is a quarter", 240 <= eff(4) <= 260, eff(4))
e.call("lira_midi_msg", 0x90, 70, 20)
check("... and a MIDI note does not touch it (TOUCH means where you hit the plate)", 240 <= eff(4) <= 260, eff(4))
e.call("lira_midi_msg", 0x80, 70, 0)
e.call("lira_ui_set", 214, 500)
check("AMT 50 %: the same quarter now counts half (1 - 0.5 x 0.75 = 0.625)", 615 <= eff(4) <= 635, eff(4))
e.call("lira_ui_set", 214, 1000)
# a real touch near the top and near the bottom of plate 5 (voice index 4)
PLX = 5 + 38 * 4 + 19
tap(PLX, 176)
top = eff(4)
tap(PLX, 176)                                                              # (latched on, then off again)
tap(PLX, 203)
bot = eff(4)
tap(PLX, 203)
check("TOUCH on the real plate: a tap near the top plays loud, near the bottom soft", top > 900 and bot < 250, (top, bot))
# MIDI
e.call("lira_ui_set", 215, 3)
e.call("lira_ui_tapvel", 4, 100)
e.call("lira_midi_msg", 0x90, 70, 127)
v127 = eff(4)
e.call("lira_midi_msg", 0x80, 70, 0)
e.call("lira_midi_msg", 0x90, 70, 40)
v40 = eff(4)
check("MIDI: a note's velocity: 127 -> full, 40 -> about a third", v127 == 1000 and 305 <= v40 <= 325, (v127, v40))
e.call("lira_midi_msg", 0x81, 70, 0)
e.call("lira_midi_msg", 0x91, 70, 0)
check("... a note off, or a note on with velocity 0, changes nothing", 305 <= eff(4) <= 325, eff(4))
e.call("lira_midi_msg", 0x90, 70, 1)
check("... velocity 1 is the softest (not zero: that is a note off)", 0 < eff(4) < 20, eff(4))
e.call("lira_midi_msg", 0x80, 70, 0)
# SLIDE: the finger's height while the plate is held
e.call("lira_ui_key", 4, 0)                                                 # (plate off to start with)
e.call("lira_ui_set", 215, 2)
touch("down", PLX, 176, up=False)
hi = eff(4)
touch("move", PLX, 203)
lo = eff(4)
touch("move", PLX, 190)
mid = eff(4)
check("SLIDE: down at the top is loud, sliding down to the bottom makes it soft, back up half way is in between (live, finger still down)", hi > 900 and lo < 250 and lo < mid < hi, (hi, mid, lo))
e.call("solo_touch_up", VIEW, PTA, 0)
check("... the latched plate stays on when the finger lifts (and keeps the last height)", key(4) == 1 and eff(4) == mid, (key(4), eff(4), mid))
tap(PLX, 176)
tap(PLX, 176)
e.call("lira_ui_set", 215, 0)
# the buttons cycle through their choices, and AMT / SOURCE / EFFECT are learnable
tap(5 + 152 + 38 + 19, 47)
check("SOURCE button: STEADY -> TOUCH", get(215) == 1, get(215))
for want_ in (2, 3, 0):
    slow_tap(5 + 152 + 38 + 19, 47)
    check("... -> %d" % want_, get(215) == want_, get(215))
e.call("lira_ui_set", 216, 0)
for want_ in (1, 2, 0):
    slow_tap(5 + 152 + 19, 85)
    check("EFFECT button: SOUND / VOLUME / BOTH cycle (%d)" % want_, get(216) == want_, get(216))
learn(4)
for cid, kind in ((214, "AMT knob"), (215, "SOURCE"), (216, "EFFECT")):
    learn(1)
    tap({214: 5 + 152 + 19, 215: 5 + 152 + 38 + 19, 216: 5 + 152 + 19}[cid], {214: 47, 215: 47, 216: 85}[cid])
    chosen = learn(6)
    e.call("lira_midi_msg", 0xB0, 90 + cid - 214, 100)
    learn(1)
    check("VELOCITY %s: a tap in learn mode chooses it and a controller binds it (id %d)" % (kind, cid), chosen == cid and learn(5, cid) == 91 + cid - 214, (chosen, learn(5, cid)))
e.call("lira_midi_msg", 0xB0, 90, 127)
check("... the AMT knob follows its controller (127 -> 1000)", get(214) == 1000, get(214))
e.call("lira_ui_set", 215, 0)
e.call("lira_midi_msg", 0xB0, 91, 0)
e.call("lira_midi_msg", 0xB0, 91, 127)
check("... SOURCE steps on a controller going up (STEADY -> TOUCH)", get(215) == 1, get(215))
e.call("lira_ui_set", 216, 0)
e.call("lira_midi_msg", 0xB0, 92, 0)
e.call("lira_midi_msg", 0xB0, 92, 127)
check("... and EFFECT (SOUND -> VOLUME)", get(216) == 1, get(216))
e.call("lira_ui_set", 215, 0)
e.call("lira_ui_set", 216, 0)
e.call("lira_ui_set", 214, 700)
learn(4)
# n15: stuck notes. A plate played by a MIDI note follows the key; all-notes-off; the USB ring never drops a note off; touch slots; gesture loops end their notes
def mom():
    e.call("lira_ui_momentary", 0)
    return e.uc.reg_read(A.UC_ARM_REG_R0)
m_orig = mom()
learn(4)
learn(1); learn(2, 302); midi(0x90, 62, 100); learn(1)           # plate 3 <- note 62 (channel 1)
learn(1); learn(2, 303); midi(0x91, 64, 100); learn(1)           # plate 4 <- note 64 on channel 2
for want_m in (1, 0):                                            # both switch positions: HOLD and LATCH
    while mom() != want_m:
        e.call("lira_ui_momentary", 1)
    e.call("lira_ui_key", 2, 0)
    midi(0x90, 62, 90)
    a = key(2)
    midi(0x80, 62, 0)
    check("note on / off (mom=%d): the plate follows the key" % mom(), a == 1 and key(2) == 0, (a, key(2)))
    midi(0x90, 62, 90)
    midi(0x90, 62, 0)
    check("... a note on with velocity 0 is an off (mom=%d)" % mom(), key(2) == 0, key(2))
    midi(0x80, 62, 0)
    check("... an off without an on changes nothing", key(2) == 0, key(2))
    midi(0x90, 62, 90)
    midi(0x90, 62, 80)
    midi(0x80, 62, 0)
    check("... two note ons then one off: let go", key(2) == 0, key(2))
    midi(0x90, 62, 90)
    midi(0x81, 62, 0)
    check("... the off came on another channel than the on: still lets go", key(2) == 0, key(2))
    midi(0x91, 64, 90)
    midi(0x80, 64, 0)
    check("... (and the other way: on channel 2, off on channel 1)", key(3) == 0, key(3))
e.call("lira_ui_key", 2, 1); e.call("lira_ui_key", 3, 1); e.call("lira_ui_key", 6, 1)
midi(0xB0, 123, 0)
check("CC 123 (all notes off) lets every plate go", [key(k) for k in range(8)] == [0] * 8, [key(k) for k in range(8)])
e.call("lira_ui_key", 5, 1)
midi(0xB3, 120, 0)
check("CC 120 (all sound off), any channel, does the same", key(5) == 0, key(5))
# the USB ring: a flood of controller changes does not push a note off out
while learn(8) and False:
    pass
usb_buffer([(0x09, 0x90, 62, 100)])
poll()
check("USB: a note on from the callback ring presses the plate", key(2) == 1, key(2))
for _ in range(4):                                                # 4 buffers of 127 controller changes: more than the ring holds, no poll in between
    usb_buffer([(0x0B, 0xB0, 74, i) for i in range(127)])
usb_buffer([(0x08, 0x80, 62, 0)])
poll()
check("USB: the ring flooded with controller changes keeps the note off (dropped CCs, not notes)", key(2) == 0, key(2))
usb_buffer([(0x09, 0x90, 62, 100)])
for _ in range(4):
    usb_buffer([(0x0B, 0xB0, 74, i) for i in range(127)])
usb_buffer([(0x0B, 0xB0, 123, 0)])
poll()
check("USB: all-notes-off survives the flood too", key(2) == 0, key(2))
poll()
usb_buffer([(0x09, 0x90, 62, 100), (0x08, 0x80, 62, 0), (0x09, 0x90, 62, 100), (0x09, 0x90, 62, 0)])
poll()
check("USB: several note on / off in one buffer end with the plate off", key(2) == 0, key(2))
# touch: a finger's slot is its own (8 of them); a lift that never came is made up for at the next touch of that slot
e.call("lira_ui_drag", 0, 0, 1, 2, 0, 0)
e.call("lira_ui_key", 2, 1)
e.call("lira_ui_drag", 5, 0, 1, 4, 0, 0)
check("touch: slot 5 and slot 1 are different fingers (8 slots)", True)
e.call("lira_ui_drag", 1, 1)
sl1 = e.uc.reg_read(A.UC_ARM_REG_R0)
check("... the other slot is untouched", sl1 == 0, sl1)
e.call("lira_ui_drag", 0, 0, 0, 0, 0, 0); e.call("lira_ui_drag", 5, 0, 0, 0, 0, 0); e.call("lira_ui_key", 2, 0)
while mom() != m_orig:
    e.call("lira_ui_momentary", 1)
learn(4)
# the switch: lira_usb_hook = 0 leaves the callback alone
e.uc.mem_write(e.sym["lira_usb_hook"], b"\x00")
c1 = learn(9)
usb_buffer([(0x0B, 0xB0, 14, 127)])
check("the switch (lira_usb_hook = 0): the callback only calls the stock copy", learn(9) == c1 and stock_copies[-1] == 0x200, (c1, learn(9)))
e.uc.mem_write(e.sym["lira_usb_hook"], b"\x01")
learn(4)
tap(5 + 152 + 38 + 19, 47)                                        # SOURCE (top-right of VELOCITY)
check("SOURCE: OFF -> TAP (the height of the touch on a plate)", get(215) == 1, get(215))
tap(5 + 152 + 19, 85)                                             # TO (bottom-left)
check("TO: DEPTH -> VOL", get(216) == 1, get(216))
tap(5 + 152 + 38 + 19, 47)
tap(5 + 152 + 19, 85)
tap(5 + 104 + 10, 215)                                            # (back to CHORD for what follows)
tap(5 + 76 + 38 + 19, 47)                                         # SHUFFLE (VOICING panel, top right: swapped with TUNE in n14)
n1 = [note(v) for v in range(8)]
check("SHUFFLE gives another voicing", n1 != n0 and sorted(n1) != sorted(n0), (n0, n1))
tap(5 + 76 + 38 + 19, 85)                                         # ROTATE (bottom-right)
n2 = [note(v) for v in range(8)]
check("ROTATE hands every note to the next voice", n2 == [n1[7]] + n1[:7], (n1, n2))
tap(5 + 156 + 10, 215)                                            # the MIX tab: TUNE (EQUAL / JUST) is the PITCH panel's bottom right now
tap(5 + 76 + 38 + 19, 85)
check("TUNING (MIX tab, PITCH panel, bottom right) switches to just intonation", chord(153) == 1)
tap(5 + 104 + 10, 215)                                            # (back to the CHORD tab)
tap(314 - 60, 7)
check("the WAVE switch in the top bar selects the sine", chord(154) == 1)
tap(5 + 76 + 19, 47)
check("the CHORD button switches chord mode off again", chord(150) == 0)
e.call("lira_ui_key", 4, 0)
tap(5 + 38 * 4 + 19, PLD)
check("the plates are there on every page (voice 5)", key(4) == 1, key(4))
e.call("lira_ui_key", 4, 0)
# the eight hardware buttons are the plates: message 0xf9, index at +0xc
button(4)
check("hardware button 5 toggles voice 5 on", key(4) == 1)
e.uc.mem_write(0x24005000, b"")
for _ in range(30):                                                 # time passes (audio blocks): looper_ticks counts them
    pass
button(4, 0xFA)
check("its release message is swallowed (no second toggle)", key(4) == 1)
button(2)
check("another button toggles its own voice", key(2) == 1 and key(4) == 1)
e.call("lira_ui_key", 4, 0)
e.call("lira_ui_key", 2, 0)
set_ticks(9000)
tap(130, 7)                                                       # the BTN switch: the round buttons go back to being the stock buttons
check("BTN off: the switch in the top bar", chord(165) == 0, chord(165))
leds.clear()
button(4)
check("... and then a round button is not a plate any more", key(4) == 0, key(4))
tap(130, 7)
check("BTN on again", chord(165) == 1)
set_ticks(9500)
button(8, 0xF4)
check("the hardware REC button arms the gesture recorder (like on the SAMPLR page)", chord(158) == 1, chord(158))
e.call("lira_ui_action", 7)
set_ticks(9900)
button(9, 0xF6)                                                     # hardware STOP / PLAY on the page: only the gesture loop's (nothing to do with none)
set_ticks(9950)
button(10, 0xF7)
check("STOP and PLAY with no loop change nothing", chord(158) == 0, chord(158))
# BTN on: button 0 is voice 1 like the others; BTN off: it is the stock PAD button (to the stock handler, whose set-screen is solo_pad_pressed)
stockmsg = []
e.stub(0x080A2E60, lambda: stockmsg.append(tuple(struct.unpack("<H", bytes(e.uc.mem_read(e.arg(1), 2))) +
                                                 struct.unpack("<I", bytes(e.uc.mem_read(e.arg(1) + 12, 4))))))
set_ticks(10500)
button(0)
check("button 0 toggles voice 1 on, nothing goes to the stock handler", key(0) == 1 and not stockmsg, (key(0), stockmsg))
set_ticks(10900)
button(0)
check("... and off again", key(0) == 0 and not stockmsg, (key(0), stockmsg))
tap(130, 7)                                                       # BTN off
set_ticks(11300)
button(0)
check("BTN off: button 0 goes to the stock handler (PAD, back to the pads) and is not a plate", key(0) == 0 and stockmsg == [(0xF9, 0)], (key(0), stockmsg))
tap(130, 7)                                                       # BTN on again
stockmsg.clear()
# INFO (SHIFT on the looper pages) moves the encoders on within their part of the page; it is no modifier here
def info():
    stockmsg.clear()
    button(0, 0xC)
    return sel()


e.call("lira_ui_sel", 0)
check("INFO on the voice squares: pair 1-2 -> 3-4 -> 5-6 -> 7-8 -> 1-2", [info() >> 4 for _ in range(4)] == [1, 6, 7, 0] and not stockmsg, (sel(), stockmsg))
e.call("lira_ui_sel", 0 | 2 << 4)
seq = [info() for _ in range(4)]
check("INFO on the panels (VOICES tab): each panel in turn, the tab kept", seq == [0x30, 0x40, 0x50, 0x20], [hex(s) for s in seq])
e.call("lira_ui_sel", 2 | 2 << 4)
seq = [info() >> 4 for _ in range(4)]
check("INFO on the MIX tab's panels: all four in turn", seq == [3, 4, 5, 2], seq)
set_ticks(11000)
button(4)
check("after INFO the round buttons are still the plates (no SHIFT mode)", key(4) == 1, key(4))
f = draw(0)
check("INFO only flashes the frame (state 2): the footer has no SHIFT here", e.r8(PAGE + 7) == 2, e.r8(PAGE + 7))
e.call("lira_ui_key", 4, 0)
e.call("lira_ui_sel", 0)
PALRAM = 0x24000000
e.uc.mem_write(PALRAM, b"".join(struct.pack("<I", 0xFF000000 + 0x010101 * i) for i in range(36)))
orig = bytes(e.uc.mem_read(PALRAM, 144))
f = draw(0)
check("the page's colours are put back after it is drawn (the stock palette is untouched)", bytes(e.uc.mem_read(PALRAM, 144)) == orig)
CLUT = 0x52001400
cl = lambda a: e.r32(a) & 0xFFFFFFFF
STOCK2 = struct.unpack("<I", open("firmware/BLACKBOX-3.1.9.bin", "rb").read()[0x080F1D80 - 0x08040000 + 8:][:4])[0]
check("the page's colours are in the DMA2D's CLUT, which the display goes through (background midnight)",
      cl(CLUT + 4 * 0x02) == 0xFF0C0C14 and cl(CLUT + 0x400 + 4 * 0x02) == 0xFF0C0C14 and cl(CLUT + 4 * 0x20) == 0xFFF09A4A,
      hex(cl(CLUT + 4 * 0x02)))
e.call("looper_guard", 0)
e.uc.mem_write(0x24070000, struct.pack("<HHHHHH", 0, 0, 10, 10, 100, 100))
e.call("looper_fill_blocked", 0, 2, 0x24070000)
check("a stock screen's first fill puts the stock colours back in the CLUT", cl(CLUT + 4 * 0x02) == STOCK2 and
      cl(CLUT + 4 * 0x20) == struct.unpack("<I", open("firmware/BLACKBOX-3.1.9.bin", "rb").read()[0x080F1D80 - 0x08040000 + 0x80:][:4])[0],
      hex(cl(CLUT + 4 * 0x02)))
e.uc.mem_write(0x52001000, struct.pack("<I", 1))
f = draw(0)
check("not while the DMA2D is busy", cl(CLUT + 4 * 0x02) == STOCK2)
e.uc.mem_write(0x52001000, struct.pack("<I", 0))
f = draw(0)
check("the next draw after it is idle", cl(CLUT + 4 * 0x02) == 0xFF0C0C14)
# the pad button on the page goes back to the stock pad screen; MIX goes to the looper's first page
screens.clear()
e.call("solo_pad_pressed", APP, 0, 0, 0)
check("PAD on the LIRA page goes back to the stock pad screen", screens == [2], screens)
if os.environ.get("LIRA_THEMES") and "voices14" in GRABS and "chord" in GRABS:
    from PIL import Image, ImageDraw
    THEMES = {
        "A slate / copper / ice": dict(bg=0x0e1116, card=0x171c24, line=0x2d3542, track=0x5b6676, label=0x93a0b3, hi=0xf2f5fa, a=0xc8794a, ad=0x5e3a22, b=0x7bb8d4, bd=0x28495a, sel=0xc3a6f0),
        "B graphite / sage / rose": dict(bg=0x121110, card=0x1b1917, line=0x332f2b, track=0x6a645d, label=0xa39b90, hi=0xf6f1ea, a=0x9bb58a, ad=0x3f4f36, b=0xd98a96, bd=0x5c3239, sel=0xf0c85a),
        "C midnight / violet / seafoam": dict(bg=0x0c0c14, card=0x15151f, line=0x2c2c3d, track=0x5d5d78, label=0x9a9ab8, hi=0xf4f4fb, a=0x9a82e8, ad=0x3b3263, b=0x6fd1b6, bd=0x21493e, sel=0xf09a4a),
        "E amber on black": dict(bg=0x0b0907, card=0x16110c, line=0x33281b, track=0x6b5636, label=0xa88a55, hi=0xfff1d6, a=0xf2b04a, ad=0x62400f, b=0xb8742a, bd=0x432a0d, sel=0x7fe0d0),
        "D mono + one accent": dict(bg=0x0d0d0e, card=0x171718, line=0x2e2e31, track=0x5f5f66, label=0x9a9aa2, hi=0xf5f5f7, a=0xd2d5da, ad=0x4b4e55, b=0x868b94, bd=0x303338, sel=0xf0a030),
    }
    sheet = Image.new("RGB", (1280, 240 * len(THEMES) + 0), (30, 30, 30))
    for row, (name, t) in enumerate(THEMES.items()):
        pal = list(PAL)
        for idx, key in ((0x02, "bg"), (0x19, "card"), (0x10, "line"), (0x09, "track"), (0x16, "label"), (0x0F, "hi"),
                         (0x1A, "a"), (0x1C, "b"), (0x1D, "ad"), (0x1F, "bd"), (0x20, "sel")):
            pal[idx] = 0xFF000000 | t[key]
        for col, view in enumerate(("voices14", "chord")):
            f_, p_ = GRABS[view]
            sheet.paste(render(f_, p_, pal), (col * 320, row * 240))
        ImageDraw.Draw(sheet).text((660, row * 240 + 4), name, fill=(255, 255, 255))
    sheet.save(os.path.join(os.environ["LIRA_PNG"], "themes.png"))
print("failures:", fails)
sys.exit(1 if fails else 0)
