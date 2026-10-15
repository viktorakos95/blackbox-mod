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


drawn_fills, drawn_pixels = [], []

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
tap(5 + 76 + 19, 85)
check("pair 2's FM Source button steps (Self -> Off)", get(105) == 1, get(105))
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
# the four encoders sit two left, two right: the left two turn voices 1 and 2, the right two voices 3 and 4
for raw, voice in ((1, 0), (0, 1), (2, 2), (3, 3)):
    before = [get(v) for v in range(8)]
    knob(raw, 80)
    after = [get(v) for v in range(8)]
    check("raw knob %d turns voice %d only" % (raw, voice + 1), after[voice] == min(1000, before[voice] + 10) and
          all(after[v] == before[v] for v in range(8) if v != voice), (before, after))
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


set_ticks(5000)
check("the encoder group is voices 1-4 (bank 0)", sel() == 0, sel())
button(11, 7)
e.call("looper_page_tab")
check("BACK on the page goes back to the TRIG page", e.uc.reg_read(A.UC_ARM_REG_R0) == 6, e.uc.reg_read(A.UC_ARM_REG_R0))
e.call("looper_page_goto", VIEW, 7)
set_ticks(5200)
button(0, 0xc)
check("INFO: voices 5-8 (bank 1)", sel() == 16, sel())
before = [get(v) for v in range(8)]
knob(1, 80)
check("... the bottom-left encoder now turns voice 5", get(4) == before[4] + 10 and get(0) == before[0], (before, [get(v) for v in range(8)]))
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
tap(5 + 62 + 10, 215)
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
drag(5 + 152 + 19, 60, 20)
drag(5 + 152 + 38 + 19, 60, 20)
check("HOLD A and HOLD B (PITCH panel) are separate knobs", 480 < get(121) < 520 and 480 < get(122) < 520, (get(121), get(122)))
# the CHORD tab
# a level knob per voice, above the tune knobs on every page
check("every voice level starts at full", all(get(170 + v) == 1000 for v in range(8)), [get(170 + v) for v in range(8)])
drag(5 + 38 + 19, 118, 150)
check("LEVEL 2 (the small knob above voice 2's tune knob): dragging down 32 px lowers it by 400", 590 < get(171) < 610 and get(170) == 1000, (get(170), get(171)))
check("... and it is the encoders' group (bank 7 is levels 5-8, bank 6 levels 1-4)", sel() >> 4 == 6, sel())
knob(1, -80)
check("the bottom-left encoder turns LEVEL 1 now", get(170) == 990 and get(171) < 610, (get(170), get(171)))
tap(5 + 38 + 19, 118)
tap(5 + 38 + 19, 118)
check("a double tap puts the level back to full", get(171) == 1000, get(171))
tap(5 + 124 + 10, 215)
check("the MIX tab: the output stage in one row", sel() == 2 | 2 << 4, sel())
d0 = get(22)
drag(5 + 19, 60, 20)
check("DRIVE (the first knob of the row): dragging up raises it", get(22) > d0, (d0, get(22)))
tap(5 + 76 + 19, 47)                                              # the SYNC panel: the LFO A button
check("the SYNC panel: tapping LFO A steps its division (OFF -> 4 BAR)", chord(144) == 1, chord(144))
knob(0, 300)                                                      # raw 0 = top-left (post-swap 1): the first cell's encoder
check("... and so does its encoder (4 BAR -> 2 BAR)", chord(144) == 2, chord(144))
tap(5 + 152 + 19, 85)                                             # the GESTURE panel: LEN (bottom-left of the third panel is UNDO, top-right LEN)
tap(5 + 152 + 38 + 19, 47)
check("the GESTURE panel: LEN steps 1 -> 2 bars", chord(148) == 1, chord(148))
tap(160, 7)
check("the REC switch in the top bar arms the recorder", chord(158) == 1, chord(158))
tap(160, 7)
check("... and a second press cancels it", chord(158) == 0, chord(158))
f = draw(0)
snap("mix")
check("MIX drawn inside the screen", all(x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224 for x in f))
# the LFO panel of SOUND: SHAPE, COMBINE, LINK, DLY SRC
tap(5 + 62 + 10, 215)
tap(5 + 228 + 19, 85)
tap(5 + 228 + 38 + 19, 85)
check("the LFO panel: LINK switches on, DLY SRC goes LFO -> self", chord(162) == 1 and chord(163) == 0, (chord(160), chord(161), chord(162), chord(163)))
tap(5 + 228 + 19, 47)
tap(5 + 228 + 38 + 19, 47)
check("SHAPE and COMBINE toggle", chord(160) == 1 and chord(161) == 1, (chord(160), chord(161)))
tap(5 + 186 + 10, 215)
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
check("FREE: an encoder click moves the voice off the note (0.05 semitone a unit)", pitch(0) % 100 != 0 and abs(pitch(0) - n0[0] * 100) == 50, pitch(0))
knob(1, -80)
check("... and back on the note it is exact again", pitch(0) == n0[0] * 100, pitch(0))
drag(5 + 19, 140, 139)
check("a drag moves it freely (0.75 semitone a pixel) ...", pitch(0) != n0[0] * 100, pitch(0))
e.call("lira_ui_pitch_set", 1, 6000 + 20)
drag(5 + 38 + 19, 140, 139)
check("... a detent sits on each note (a pixel that lands 5 cents from one snaps onto it)", pitch(1) == 6100, pitch(1))
tap(180, 7)
f = draw(0)
snap("chord")
check("CHORD drawn inside the screen", all(x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224 for x in f))
tap(5 + 76 + 19, 85)                                              # SHUFFLE (bottom-left of VOICING)
n1 = [note(v) for v in range(8)]
check("SHUFFLE gives another voicing", n1 != n0 and sorted(n1) != sorted(n0), (n0, n1))
tap(5 + 76 + 38 + 19, 85)                                         # ROTATE (bottom-right)
n2 = [note(v) for v in range(8)]
check("ROTATE hands every note to the next voice", n2 == [n1[7]] + n1[:7], (n1, n2))
tap(5 + 76 + 38 + 19, 47)                                         # TUNING (top-right)
check("TUNING switches to just intonation", chord(153) == 1)
tap(314 - 60, 7)
check("the WAVE switch in the top bar selects the sine", chord(154) == 1)
tap(5 + 76 + 19, 47)
check("the CHORD button switches chord mode off again", chord(150) == 0)
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
check("INFO on the tune knobs: voices 1-4 -> 5-8 -> 1-4", [info() >> 4, info() >> 4] == [1, 0] and not stockmsg, (sel(), stockmsg))
e.call("lira_ui_sel", 6 << 4)
check("INFO on the level knobs: voices 1-4 -> 5-8 -> 1-4", [info() >> 4, info() >> 4] == [7, 6], sel())
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
