"""The MCL page's MegaCommand layout (src/cpu.c mcl_page_*) under Unicorn, from out/cave.elf alone (no stock image needed).

Run ./build.sh first. A fake blob entry table (abi 5) stands in for MCL: its screen is a test picture, its input() calls are recorded.
The page's drawing primitives (lp_box / lp_frame / lp_text_c, src/looper_page.c) are recorded and painted into a PNG preview
(out/mcl_page.png, colours approximate: the stock palette is not in this repository).

Checked: the screen is blitted at exactly 2x (every lit pixel a 2x2 block, nothing else in the screen area), the name plate (red mark,
bold MACHINEDRUM; no MEGACOMMAND), the four buttons and their touch areas (B1 NO / B2 PAGE / B3 SHIFT / B4 YES, press and release), the shortcuts
(CHROMATIC with its piano, DELAY and REVERB with MCL's own RHYTHM ECHO / GATE BOX icons, MIXER: input kind 6, the one showing lit from midi(10)), the clock on the footer line (CLOCK,
PLAY / STOP, - / +: kinds 4 / 5 and their arguments, the tempo limits), nothing past the page's width or across the footer line, no
overlaps, no restart record over the screen, the 'MCL is off' prompt and its tap, and that a blob of abi 4 gets neither strip nor clock.
NOT checked (hardware only): the real colours, touch feel, the clock itself (test_mcl_clock.py and the unit).
"""
import os
import struct
import sys
import tempfile

from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_SP

from emu import BASE, STACK, Emu

ENTRY, STUBS, FB = 0x24070000, 0x24071000, 0x24072000
UI = 0x38800930
W, FOOT = 314, 224                       # the page as the unit gives it (test_mcl_ui.py)
fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


img = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
img.write(bytes(0x08120000 - BASE))
img.close()
e = Emu(img.name, "out/cave.elf")
os.unlink(img.name)
u = e.uc
u.mem_write(0x080F1E78, open("out/cave.bin", "rb").read())
u.mem_map(0x38800000, 0x1000)            # backup SRAM: the page's state
u.mem_map(0x58020000, 0x10000)           # RCC / PWR (bkp_enable)

# --- the fake blob: an entry table and its functions (each a hooked `bx lr`)
calls, state = [], {"sig": 7, "fb": FB, "bpm_seen": 0, "boots": 0, "page": 255}
fns = {}


def fn(name, handler):
    addr = STUBS + 4 * len(fns)
    u.mem_write(addr, b"\x70\x47\x00\xbf")
    e.stub(addr, handler)
    fns[name] = addr | 1


def i32(v):
    return struct.unpack("<i", struct.pack("<I", v))[0]


fn("boot", lambda: (state.__setitem__("boots", state["boots"] + 1), e.ret(0)))
fn("status", lambda: e.ret(0))
fn("gui", lambda: e.ret(0))
fn("fb", lambda: e.ret(state["fb"]))
fn("sig", lambda: e.ret(state["sig"]))
fn("input", lambda: (calls.append((e.arg(0), i32(e.arg(1)), i32(e.arg(2)))), e.ret(0)))
fn("midi", lambda: e.ret(state["bpm_seen"] if e.arg(0) == 6 else state["page"] if e.arg(0) == 10 else 0))


def entry(abi):
    u.mem_write(ENTRY, struct.pack("<10I", 0x424C434D, abi, fns["boot"], fns["status"], fns["gui"], 0, fns["fb"], fns["sig"], fns["input"], fns["midi"]))
    e.w32(e.sym["mcl_entry"], ENTRY)


entry(5)

# MCL's screen: a test picture (a frame, a filled block, a diagonal, a row of single pixels)
pic = [[0] * 128 for _ in range(64)]
for x in range(128):
    pic[0][x] = pic[63][x] = 1
for y in range(64):
    pic[y][0] = pic[y][127] = 1
for y in range(8, 24):
    for x in range(8, 40):
        pic[y][x] = 1
for i in range(40):
    pic[20 + i // 2][50 + i] = 1
for x in range(60, 120, 2):
    pic[40][x] = 1
fbb = bytearray(1024)
for y in range(64):
    for x in range(128):
        if pic[y][x]:
            fbb[y * 16 + (x >> 3)] |= 0x80 >> (x & 7)
u.mem_write(FB, bytes(fbb))

rec = []
for name in ("lp_box", "lp_frame", "lp_text_c"):
    e.stub(e.sym[name], lambda n=name: rec.append((n,) + tuple(e.arg(i) for i in range(4)) + (
        (e.r32(e.uc.reg_read(UC_ARM_REG_SP)), e.r32(e.uc.reg_read(UC_ARM_REG_SP) + 4)) if n != "lp_box" else (e.r32(e.uc.reg_read(UC_ARM_REG_SP)),)) +
        ((e.cstr(e.arg(3)),) if n == "lp_text_c" else ())))


def draw():
    rec.clear()
    e.call("mcl_page_draw", W, FOOT)
    page = len(rec)
    e.call("mcl_page_foot", W, FOOT)
    draw.page = page
    return list(rec)


def page_part(c):
    return c[:draw.page]


def foot_part(c):
    return c[draw.page:]


def ui():
    b = bytes(u.mem_read(UI, 20))
    return {"mask": struct.unpack("<H", b[4:6])[0], "off": struct.unpack("<H", b[14:16])[0], "clk": b[16], "bpm": struct.unpack("<H", b[18:20])[0]}


def page_down(tid, x, d):
    e.w32(STACK, FOOT)                    # the fifth argument (foot_y) is on the stack: e.call starts with SP = STACK
    e.call("mcl_page_down", tid, x, d, W)


def touch(x, d, tid=0):
    calls.clear()
    page_down(tid, x, d)
    down = list(calls)
    calls.clear()
    e.call("mcl_page_up", tid)
    return down, list(calls)


u.mem_write(UI, bytes(32))
c = draw()
boxes = [r for r in c if r[0] == "lp_box"]
texts = [r for r in c if r[0] == "lp_text_c"]
x0, TOP = (W - 256) // 2, 31
lit = [(x, y) for y in range(64) for x in range(128) if pic[y][x]]
white = [r for r in boxes if r[5] == 0x0F and TOP <= r[2] < TOP + 128 and x0 <= r[1] < x0 + 256]
cover = set()
for _, x, d, w, h, col in white:
    for yy in range(d, d + h):
        for xx in range(x, x + w):
            cover.add((xx, yy))
want = {(x0 + 2 * x + i, TOP + 2 * y + j) for x, y in lit for i in (0, 1) for j in (0, 1)}
check("the screen is exactly 2x: the lit runs cover each lit MCL pixel as a 2x2 block, nothing else", cover == want, (len(cover), len(want)))
check("every run is 2 px high at an even offset (no 2.4x rounding)", all(r[4] == 2 and (r[2] - TOP) % 2 == 0 and (r[1] - x0) % 2 == 0 for r in white))
names = [r[-1] for r in texts]
check("the panel's printing: MACHINEDRUM, SAVE NO PAGE / LOAD YES SHIFT", all(s in names for s in
      ("MACHINEDRUM", "SAVE", "NO", "PAGE", "LOAD", "YES", "SHIFT")), names)
check("no MEGACOMMAND anywhere", not any("MEGACOMMAND" in s for s in names), names)
check("MACHINEDRUM in bold (drawn twice, one pixel apart)", sorted(r[1] for r in texts if r[-1] == "MACHINEDRUM")[1] -
      sorted(r[1] for r in texts if r[-1] == "MACHINEDRUM")[0] == 1)
plate = [r for r in texts if r[-1] == "MACHINEDRUM"][0]
logo = [r for r in boxes if r[5] == 0x06 and 17 <= r[2] < 31 and r[1] > 26]          # (inside the window: not the red NO cap)
check("the red mark sits left of the name, on its line", logo and all(r[1] + r[3] <= plate[1] and plate[2] <= r[2] < plate[2] + 8 for r in logo), logo[:3])
check("the shortcuts (abi 5): CHROMATIC DELAY REVERB MIXER", all(s in names for s in ("CHROMATIC", "DELAY", "REVERB", "MIXER")), names)
check("no MIDI / error counters, no restart record", not any("ERR" in s or "RESTART" in s for s in names), names)
fnames = [r[-1] for r in foot_part(c) if r[0] == "lp_text_c"]
check("the footer: CLOCK BLACKBOX, PLAY, - / +, the tempo, the knobs", all(s in fnames for s in ("CLOCK BLACKBOX", "PLAY", "-", "+", "--", "KNOBS = A-D")), fnames)


def extent(r):
    """(left, top, right, bottom) of what a call paints; text is centred in its width, 6 px a character, 8 high, times its scale"""
    if r[0] == "lp_text_c":
        sc = r[6]
        tw = len(r[-1]) * 6 * sc - sc
        left = r[1] + (r[3] - tw) // 2
        return left, r[2], left + tw, r[2] + 8 * sc
    return r[1], r[2], r[1] + r[3], r[2] + r[4]


def inside(r, top, bottom):
    x = extent(r)
    return x[0] >= 1 and x[2] <= W - 1 and x[1] >= top and x[3] <= bottom


bad = [r for r in page_part(c) if not inside(r, 16, FOOT)] + [r for r in foot_part(c) if not inside(r, FOOT + 1, FOOT + 14)]
check("the page stays above the footer line, the footer inside its 14 px, nothing past the width", not bad, bad[:3])


def overlap(a, b):
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


labels = [extent(r) for r in c if r[0] == "lp_text_c" and r[-1] in ("PAGE", "SHIFT", "NO", "YES", "SAVE", "LOAD")]
others = [extent(r) for r in c if r[0] == "lp_box" and r[5] != 0x1C]
check("the buttons' names overlap nothing drawn (the window, the shortcuts, the caps)", not [(a, b) for a in labels for b in others if overlap(a, b)],
      [(a, b) for a in labels for b in others if overlap(a, b)][:3])
fwords = [(r[-1], extent(r)) for r in foot_part(c) if r[0] == "lp_text_c"]
check("the footer's words do not overlap", not [(a, b) for i, a in enumerate(fwords) for b in fwords[i + 1:] if overlap(a[1], b[1])])

# --- the buttons: B1 NO (left, upper), B2 PAGE (left, lower), B3 SHIFT (right, lower), B4 YES (right, upper)
for (x, d), b, label in (((10, 50), 0, "NO"), ((10, 150), 1, "PAGE"), ((W - 10, 150), 2, "SHIFT"), ((W - 10, 50), 3, "YES")):
    down, up = touch(x, d, 2)
    check(f"{label}: press / release send button {b + 1}", down == [(2, b, 0)] and up == [(3, b, 0)], (down, up))
page_down(1, 10, 50)
check("a held button shows in the mask", ui()["mask"] == 1, ui())
held = [r for r in draw() if r[0] == "lp_frame" and r[5] == 0x1B]
check("... and is drawn held", len(held) == 1, held)
e.call("mcl_page_up", 1)
check("touching MCL's screen does nothing", touch(W // 2, 80) == ([], []))

# --- the shortcuts
STRIP_Y = 17 + 145 + 4                   # under the window (name plate 14, screen 128, edge 3)
STRIP_H = 48
STRIP = STRIP_Y + 10


def tile(label, foot=False):
    r = next(r for r in (foot_part(draw()) if foot else page_part(draw())) if r[0] == "lp_text_c" and r[-1] == label and (foot or r[2] >= STRIP_Y))
    return r[1] + r[3] // 2


def strip_fits():
    c = page_part(draw())
    tiles = [extent(r) for r in c if r[0] == "lp_box" and r[2] == STRIP_Y]
    words = [(r[-1], extent(r)) for r in c if STRIP_Y < r[2] < STRIP_Y + STRIP_H and r[0] != "lp_frame" and r[5] != 0x1C]   # (the tiles' own boxes start at STRIP_Y)
    return [w for w, x in words if not any(t[0] + 2 <= x[0] and x[2] <= t[2] - 2 and t[1] + 2 <= x[1] and x[3] <= t[3] - 2 for t in tiles)]


check("each shortcut's name and picture fit inside its tile's frame", not strip_fits(), strip_fits()[:4])
for k, name in enumerate(("CHROMATIC", "DELAY", "REVERB", "MIXER")):
    down, up = touch(tile(name), STRIP)
    check(f"{name}: shortcut {k} (kind 6), nothing on release", down == [(6, k, 0)] and up == [], (down, up))
piano = [r for r in page_part(draw()) if r[0] == "lp_box" and r[5] == 0x0E and STRIP_Y + 10 <= r[2] < STRIP_Y + 22 and r[4] == 7]
check("the chromatic tile has a little piano (4 black keys)", len(piano) == 4, piano)


def icon_pixels(name):
    """MCL's own bitmap (the fork's resource/icons_page.cpp, as copied into src/cpu.c) -> its lit (x, y)"""
    src = open("src/cpu.c").read()
    body = src[src.index(name + "[75] = {"):]
    b = [int(t, 16) for t in body[body.index("{") + 1:body.index("}")].replace(",", " ").split()]
    return {(x, y) for y in range(25) for x in range(24) if b[y * 3 + x // 8] >> (7 - x % 8) & 1}


c = page_part(draw())
for k, name, arr in ((1, "DELAY", "icon_echo"), (2, "REVERB", "icon_gbox")):
    tiles = sorted(r[1] for r in c if r[0] == "lp_box" and r[2] == STRIP_Y)
    t0 = tiles[k]
    got = set()
    for r in c:
        if r[0] == "lp_box" and r[5] == 0x0F and STRIP_Y < r[2] < STRIP_Y + 30 and t0 <= r[1] < t0 + 52 and r[4] == 1:
            for xx in range(r[1], r[1] + r[3]):
                got.add((xx, r[2]))
    ox, oy = min(x for x, _ in got) - min(x for x, _ in icon_pixels(arr)), min(y for _, y in got) - min(y for _, y in icon_pixels(arr))
    check(f"{name}: MCL's {arr} drawn pixel for pixel", got == {(x + ox, y + oy) for x, y in icon_pixels(arr)}, (len(got), len(icon_pixels(arr))))
state["page"] = 2
lit = [r[-1] for r in page_part(draw()) if r[0] == "lp_text_c" and r[5] == 0x1B]
check("the page MCL shows (midi(10) = 2) lights its shortcut: REVERB", lit == ["REVERB"], lit)
e.call("mcl_page_sig")
s1 = e.uc.reg_read(UC_ARM_REG_R0)
state["page"] = 3
e.call("mcl_page_sig")
check("... and a page change changes the signature", e.uc.reg_read(UC_ARM_REG_R0) != s1)
state["page"] = 255

# --- the clock on the footer line
FD = FOOT + 6
play = tile("PLAY", True)
down, _ = touch(play, FD)
check("PLAY from the Blackbox's clock: MCL's own tempo, running, 120.0", down == [(4, 1, 1200)] and ui()["clk"] == 1, (down, ui()))
fn_ = [r[-1] for r in foot_part(draw()) if r[0] == "lp_text_c"]
check("... the footer shows STOP, CLOCK INTERNAL, 120.0", all(s in fn_ for s in ("STOP", "CLOCK INTERNAL", "120.0")), fn_)
plus = tile("+", True)
down, _ = touch(plus, FD)
check("+ : 121.0 (kind 5)", down == [(5, 0, 1210)] and ui()["bpm"] == 1210, (down, ui()))
for _ in range(400):
    touch(plus, FD)
check("the tempo stops at 300.0", ui()["bpm"] == 3000, ui())
fwords = [(r[-1], extent(r)) for r in foot_part(draw()) if r[0] == "lp_text_c"]
check("... 300.0 still fits (no overlap on the footer)", not [(a, b) for i, a in enumerate(fwords) for b in fwords[i + 1:] if overlap(a[1], b[1])], fwords)
minus = tile("-", True)
for _ in range(400):
    touch(minus, FD)
check("... and at 30.0", ui()["bpm"] == 300, ui())
down, _ = touch(tile("STOP", True), FD)
check("STOP: own tempo, stopped", down == [(4, 2, 300)] and ui()["clk"] == 2, (down, ui()))
down, _ = touch(tile("PLAY", True), FD)
check("PLAY again: running", down == [(4, 1, 300)], down)
down, _ = touch(tile("CLOCK INTERNAL", True), FD)
check("CLOCK: back to the Blackbox's sequencer", down == [(4, 0, 300)] and ui()["clk"] == 0, (down, ui()))
down, _ = touch(tile("CLOCK BLACKBOX", True), FD)
check("CLOCK again: own tempo, stopped (PLAY starts it)", down == [(4, 2, 300)] and ui()["clk"] == 2, (down, ui()))
touch(tile("CLOCK INTERNAL", True), FD)
down, _ = touch(minus, FD)
check("- / + do nothing while following the Blackbox", down == [] and ui()["bpm"] == 300, (down, ui()))
check("the tempo readout and the knob note are not buttons", touch(tile("--", True), FD) == ([], []) and touch(tile("KNOBS = A-D", True), FD) == ([], []))
state["bpm_seen"] = 1234
check("following: the footer shows the tempo the Blackbox gives (123.4)", "123.4" in [r[-1] for r in draw() if r[0] == "lp_text_c"])
e.call("mcl_page_sig")
s1 = e.uc.reg_read(UC_ARM_REG_R0)
state["bpm_seen"] = 1300
e.call("mcl_page_sig")
check("... and a new tempo changes the page signature", e.uc.reg_read(UC_ARM_REG_R0) != s1)
state["bpm_seen"] = 0


# --- the preview picture (before the abi 4 / off states below)
def preview(path, c):
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return
    pal = {0x1C: (185, 188, 193), 0x1E: (43, 45, 49), 0x0E: (0, 0, 0), 0x0F: (240, 240, 240), 0x06: (229, 72, 77), 0x10: (90, 93, 100),
           0x1B: (124, 200, 240), 0x0B: (95, 207, 143), 0x09: (120, 125, 135)}
    src = open("src/mcl_bb/glcdfont.h").read()
    body = src[src.index("{") + 1:src.rindex("}")]
    font = [int(t, 16) for t in body.replace("//", " ").replace(",", " ").split() if t.startswith("0x")]
    S = 3
    im = Image.new("RGB", (W * S, 240 * S), (20, 20, 20))
    g = ImageDraw.Draw(im)
    g.rectangle([0, 0, W * S, 15 * S], fill=(60, 60, 64))

    def box(x, d, w, h, col):
        if w > 0 and h > 0:
            g.rectangle([x * S, d * S, (x + w) * S - 1, (d + h) * S - 1], fill=pal.get(col, (255, 0, 255)))

    for r in c:
        if r[0] == "lp_box":
            box(r[1], r[2], r[3], r[4], r[5])
        elif r[0] == "lp_frame":
            x, d, w, h, col, th = r[1:7]
            box(x, d, w, th, col); box(x, d + h - th, w, th, col); box(x, d, th, h, col); box(x + w - th, d, th, h, col)
        else:
            x, d, w, _, col, sc, s = r[1:8]
            tw = len(s) * 6 * sc - sc
            cx = x + (w - tw) // 2
            for i, ch in enumerate(s):
                for col_i in range(5):
                    bits = font[ord(ch) * 5 + col_i]
                    for row in range(8):
                        if bits >> row & 1:
                            box(cx + (i * 6 + col_i) * sc, d + row * sc, sc, sc, col)
    im.save(path)
    print("preview:", path)


touch(tile("PLAY", True), FD)
for _ in range(4):
    touch(tile("+", True), FD)
state["page"] = 0
preview("out/mcl_page.png", draw())
state["page"] = 255
touch(tile("STOP", True), FD)

# --- 'MCL is off' (the Blackbox restarted without a power-on): a short prompt in the window, a tap starts MCL
u.mem_write(UI + 14, struct.pack("<H", 1))
names = [r[-1] for r in draw() if r[0] == "lp_text_c"]
check("off: 'MCL IS OFF' / 'TAP HERE TO START', no picture", "MCL IS OFF" in names and "TAP HERE TO START" in names and
      not [r for r in rec if r[0] == "lp_box" and r[4] == 2], names)
calls.clear()
page_down(0, 10, 50)
check("... a tap on the NO button does nothing while off", calls == [] and state["boots"] == 0 and ui()["off"] == 1)
e.call("mcl_page_up", 0)
page_down(0, W // 2, 90)
check("... a tap on the window starts MCL", state["boots"] == 1 and ui()["off"] == 0, (state, ui()))
e.call("mcl_page_up", 0)

# --- an older blob (abi 4): the same panel, no shortcuts, no clock: the footer says what the knobs are
entry(4)
c = draw()
names = [r[-1] for r in c if r[0] == "lp_text_c"]
check("abi 4: no shortcuts, no clock", "CHROMATIC" not in names and "PLAY" not in names and "NO" in names and
      "MCL   KNOBS 1-4 = ENCODERS A-D" in names, names)
check("... taps where they would be do nothing", touch(W // 2, STRIP) == ([], []) and touch(play, FD) == ([], []))

print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
