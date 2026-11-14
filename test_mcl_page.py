"""The MCL page's MegaCommand layout (src/cpu.c mcl_page_*) under Unicorn, from out/cave.elf alone (no stock image needed).

Run ./build.sh first. A fake blob entry table (abi 5) stands in for MCL: its screen is a test picture, its input() calls are recorded.
The page's drawing primitives (lp_box / lp_frame / lp_text_c, src/looper_page.c) are recorded and painted into a PNG preview
(out/mcl_page.png, colours approximate: the stock palette is not in this repository).

Checked: the screen is blitted at exactly 2x (every lit pixel a 2x2 block, nothing else in the screen area), the name plate (the Machinedrum's mark,
MACHINEDRUM at 1.5x in bold from the firmware's font, pixel for pixel; no MEGACOMMAND), the four buttons and their touch areas (B1 NO / B2 PAGE /
B3 SHIFT / B4 YES, press and release), the shortcuts (CHROMATIC with its piano, DELAY and REVERB with MCL's own RHYTHM ECHO / GATE BOX icons, MIXER,
GRID: input kind 6, the one showing lit from midi(10)), the footer line (only what the knobs are; no clock, taps there do nothing), nothing past
the page's width or across the footer line, no overlaps, no restart record over the screen, and that a blob
of abi 4 gets no strip.
NOT checked (hardware only): the real colours, touch feel.
"""
import os
import struct
import sys
import tempfile

from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_SP

from emu import BASE, STACK, Emu

ENTRY, STUBS, FB = 0x24070000, 0x24071000, 0x24072000
UI = 0x38800680
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

# the firmware's 6 x 8 text font (struct {data, w, h} at 0x240000d0; a byte a row, leftmost pixel in the top bit), made from glcdfont.h
_src = open("src/mcl_bb/glcdfont.h").read()
_body = _src[_src.index("{") + 1:_src.rindex("}")]
GLCD = [int(t, 16) for t in _body.replace("//", " ").replace(",", " ").split() if t.startswith("0x")]
FONT_ROWS = bytearray()
for ch in range(0x20, 0x7F):
    for r in range(8):
        FONT_ROWS.append(sum(0x80 >> k for k in range(5) if GLCD[ch * 5 + k] >> r & 1))
FONT_AT = 0x24074000
u.mem_write(FONT_AT, bytes(FONT_ROWS))
u.mem_write(0x240000D0, struct.pack("<IHH", FONT_AT, 6, 8))

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
GRID = {"on": False}
GNAMES = ["BD", "SD", "HH", "CP", "RS", "CB", "CY", "LT", "MT", "HT", "TM", "--", "--", "--", "--", "--"]


def grid_data():
    """struct bb_grid (src/mcl_bb/bb_grid.h): cursor track 2 row 1, a 2 x 1 selection, grid X, running, rows from A05, 125.0 BPM, bar 3 beat 2"""
    b = struct.pack("<6BHH2B", 2, 1, 2, 1, 0, 2, 4, 1250, 3, 2)
    b += bytes([5 if k in (0, 1) else 6 if k == 2 else 0xFF for k in range(16)])                 # tracks 1, 2 play row A06 (view row 1), track 3 A07
    for r in range(4):
        b += b"".join((GNAMES[(k + r) % 16]).encode() for k in range(16))
    for r in range(4):
        b += (b"INTRO" if r == 1 else b"").ljust(17, b"\0")
    b += b"MD\0" + b"MI\0"
    return b


def grid_fn():
    if GRID["on"]:
        u.mem_write(e.arg(0), grid_data())
    e.ret(1 if GRID["on"] else 0)


fn("grid", grid_fn)
MIXER = {"on": False}


def mixer_data():
    """struct bb_mixer: FLTF (param 12), 16 tracks, tracks 3 and 4 held, track 6 muted, the roll armed at 1/8"""
    vals = [10 + 7 * k for k in range(16)]
    mets = [v // 2 for v in vals]
    return struct.pack("<BBHHBB", 12, 16, (1 << 2) | (1 << 3), 1 << 5, 1, 0) + bytes(vals) + bytes(mets) + b"FLTF".ljust(8, b"\0") + b"1/8".ljust(6, b"\0")


def mixer_fn():
    if MIXER["on"]:
        u.mem_write(e.arg(0), mixer_data())
    e.ret(1 if MIXER["on"] else 0)


fn("mixer", mixer_fn)
PLAYER, PRING = 0x24076000, 0x24076100
fn("player", lambda: e.ret(PLAYER))


def entry(abi):
    u.mem_write(ENTRY, struct.pack("<13I", 0x424C434D, abi, fns["boot"], fns["status"], fns["gui"], 0, fns["fb"], fns["sig"], fns["input"], fns["midi"], fns["grid"], fns["mixer"], fns["player"]))
    e.w32(e.sym["mcl_entry"], ENTRY)


entry(7)

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
# lp_text(x, d, s, color, scale): recorded as ("lp_text", x, d, s, color, scale)
e.stub(e.sym["lp_text"], lambda: rec.append(("lp_text", i32(e.arg(0)), i32(e.arg(1)), e.cstr(e.arg(2)), e.arg(3), e.r32(e.uc.reg_read(UC_ARM_REG_SP)))))


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
    return {"mask": struct.unpack("<H", b[4:6])[0], "off": struct.unpack("<H", b[14:16])[0], "quick": b[16]}


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
x0, TOP = (W - 256) // 2, 17 + 19
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
check("the panel's printing: SAVE NO PAGE / LOAD YES SHIFT", all(s in names for s in ("SAVE", "NO", "PAGE", "LOAD", "YES", "SHIFT")), names)
check("no MEGACOMMAND anywhere", not any("MEGACOMMAND" in s for s in names), names)
plate_px = set()
for r in boxes:
    if r[5] == 0x0F and 17 <= r[2] < TOP and 26 < r[1] < W - 26:            # (inside the window: not the white YES cap)
        for yy in range(r[2], r[2] + r[4]):
            for xx in range(r[1], r[1] + r[3]):
                plate_px.add((xx, yy))
px0 = min(x for x, _ in plate_px)
want_px = set()
for i, ch in enumerate("MACHINEDRUM"):
    for rr in range(8):
        for k in range(5):
            if GLCD[ord(ch) * 5 + k] >> rr & 1:
                for yy in range(rr * 3 // 2, (rr + 1) * 3 // 2):
                    for xx in range(k * 3 // 2, (k + 1) * 3 // 2 + 1):
                        want_px.add((px0 + i * 9 + xx, 17 + 3 + yy))
check("MACHINEDRUM on the plate at 1.5x, bold (9 x 12 a character), pixel for pixel", plate_px == want_px, (len(plate_px), len(want_px)))
LOGO = ["........OOOOOOOOOOO.........RRRRRRR", ".......OOOOOOOOOOOO.........RRRRRRR", "......OOOOOOOOOOOOO........RRRRRRR.",
        ".....OOOOOOOOOOOOOO........RRRRRRR.", "....OOOOOOOOOOOOOOO.......RRRRRRR..", "...OOOOOOOOOOOOOOOO.......RRRRRRR..",
        "..OOOOOOOOO.OOOOOOO......RRRRRRR...", ".OOOOOOOOO..OOOOOOO......RRRRRRR...", "OOOOOOOOO...OOOOOOO...RRRRRRRRR....",
        "OOOOOOO.....OOOOOOO...RRRRRRRRR....", ".OOOOO......OOOOOOO..RRRRRRRRR.....", "...OOO......OOOOOOO..RRRRRRRRR.....",
        "............OOOOOOO.RRRRRRRRR......"]          # the Machinedrum's mark (its front panel), O orange, R red
got_logo = {}
for r in boxes:
    if r[5] in (0x1D, 0x1F) and 17 <= r[2] < TOP:
        for xx in range(r[1], r[1] + r[3]):
            got_logo[(xx, r[2])] = "O" if r[5] == 0x1D else "R"
lx = min(x for x, _ in got_logo) if got_logo else 0
want_logo = {(lx + k, 17 + 3 + rr): ch for rr, row in enumerate(LOGO) for k, ch in enumerate(row) if ch != "."}
check("the Machinedrum's mark (orange and red, 35 x 13) pixel for pixel, left of the name, on its rows", got_logo == want_logo and lx + 35 <= px0,
      (len(got_logo), len(want_logo), lx, px0))
check("the shortcuts (abi 5): CHROMATIC DELAY REVERB MIXER GRID SAMPLE", all(s in names for s in ("CHROMATIC", "DELAY", "REVERB", "MIXER", "GRID", "SAMPLE")), names)
check("no MIDI / error counters, no restart record", not any("ERR" in s or "RESTART" in s for s in names), names)
fnames = [r[-1] for r in foot_part(c) if r[0] == "lp_text_c"]
check("the footer: empty (no clock, no text)", not foot_part(c), foot_part(c)[:3])


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
STRIP_Y = 17 + 150 + 12                  # under the window (name plate 19, screen 128, edge 3) and the PAGE / SHIFT names
STRIP_H = 43
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
for k, name in enumerate(("CHROMATIC", "DELAY", "REVERB", "MIXER", "GRID", "SAMPLE")):
    down, up = touch(tile(name), STRIP)
    check(f"{name}: shortcut {k} (kind 6), nothing on release", down == [(6, k, 0)] and up == [], (down, up))
piano = [r for r in page_part(draw()) if r[0] == "lp_box" and r[5] == 0x0E and STRIP_Y + 8 <= r[2] < STRIP_Y + 22 and r[4] == 7]
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
        if r[0] == "lp_box" and r[5] == 0x0F and STRIP_Y < r[2] < STRIP_Y + 30 and t0 <= r[1] < t0 + 44 and r[4] == 1:
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


# --- MCL's grid page at full resolution (abi 6): drawn by the page from grid(), MCL's own picture not blitted
GRID["on"] = True
c = page_part(draw())
GRID["on"] = False
lt = [r for r in c if r[0] == "lp_text"]
words = [r[3] for r in lt]
GX = x0 + (256 - 16 * 15 - 9) // 2
CX = lambda k: GX + k * 15 + k // 4 * 3                          # track k's column (3 px more at each group of four)
check("grid page: no 2x picture is blitted (no 2-px runs under the title bar)", not [r for r in c if r[0] == "lp_box" and r[4] == 2 and TOP + 14 <= r[2] < TOP + 128], [r for r in c if r[0] == "lp_box" and r[4] == 2 and TOP + 14 <= r[2] < TOP + 128][:3])
check("... the title bar: GRID X, the cursor's row A06, the devices, bar:beat 3:2 (black on white)",
      ("GRID X  A06  MD MI" in words) and "3:2" in words and all(r[4] == 0x0E for r in lt if r[3] in ("GRID X  A06  MD MI", "3:2")), words[:4])
check("... the tempo 125.0 at 2x, BPM", any(r[3] == "125.0" and r[5] == 2 for r in lt) and "BPM" in words, words[:8])
nm = [r for r in lt if r[3] == "INTRO"]
check("... the cursor's row name big and bold (2x, drawn twice one pixel apart), right-aligned in the tempo line",
      len(nm) == 2 and all(r[5] == 2 and r[4] == 0x0F for r in nm) and abs(nm[0][1] - nm[1][1]) == 1 and max(r[1] for r in nm) + 5 * 12 <= x0 + 256 - 5, nm)
nums = {r[3]: r[1] for r in lt if r[3].isdigit() and r[2] == TOP + 35}
check("... the track numbers 1..16, each centred over its column's label (centre at column + 7)",
      sorted(map(int, nums)) == list(range(1, 17)) and all(abs(nums[str(k)] + len(str(k)) * 6 / 2 - (CX(k - 1) + 7)) <= 1 for k in range(1, 17)),
      {k: nums[k] for k in list(nums)[:12]})
cells = {}
for r in lt:
    if r[2] >= TOP + 35 + 13:
        k, row = next(k for k in range(16) if CX(k) <= r[1] - 1 < CX(k) + 15), (r[2] - (TOP + 35 + 13)) // 20
        cells[(k, row)] = (r[3], r[4])
check("... all 16 tracks x 4 rows of labels, in their columns", len(cells) == 64 and all(cells[(k, rr)][0] == GNAMES[(k + rr) % 16] for k in range(16) for rr in range(4)),
      (len(cells), [cells.get((k, 0)) for k in range(4)]))
check("... the cursor and its selection (tracks 3-4, row 2) inverted: black text, the rest white, empty slots grey",
      cells[(2, 1)][1] == 0x0E and cells[(3, 1)][1] == 0x0E and cells[(4, 1)][1] == 0x0F and cells[(11 - 1, 1)][1] in (0x0F, 0x09) and
      all(cells[(k, rr)][1] == 0x09 for k in range(16) for rr in range(4) if cells[(k, rr)][0] == "--" and not (rr == 1 and k in (2, 3))), [cells[(k, 1)] for k in range(2, 5)])
under = sorted((next(k for k in range(16) if CX(k) <= r[1] < CX(k) + 15), (r[2] - (TOP + 35 + 13 + 10)) // 20) for r in c if r[0] == "lp_box" and r[3] == 12 and r[4] == 1 and TOP + 40 < r[2] < TOP + 128)
check("... the playing slots underlined: tracks 1, 2 on row 2 (A06), track 3 on row 3 (A07)", under == [(0, 1), (1, 1), (2, 2)], under)
divs = sorted({r[1] for r in c if r[0] == "lp_box" and r[3] == 1 and r[4] == 1 and TOP + 60 < r[2] < TOP + 120 and x0 < r[1] < x0 + 255})
check("... the three dotted dividers sit 2+ px clear of the labels either side", len(divs) == 3 and all(CX(4 * g) - 1 - (CX(4 * g - 1) + 12) >= 2 and CX(4 * g) + 1 - d >= 2 and d - (CX(4 * g - 1) + 12) >= 1 for g, d in zip((1, 2, 3), divs)), divs)
check("... everything inside the 256 x 128 screen area", all(x0 <= r[1] and r[1] + len(r[3]) * 6 * r[5] <= x0 + 256 and TOP <= r[2] <= TOP + 128 - 8 for r in lt),
      [r for r in lt if not (x0 <= r[1] and r[1] + len(r[3]) * 6 * r[5] <= x0 + 256)][:3])
preview("out/mcl_grid.png", page_part(draw()) if False else None) if False else None
GRID["on"] = True
_g = draw()
GRID["on"] = False
GRID_PREVIEW = _g


# --- MCL's mixer at full resolution (abi 6)
MIXER["on"] = True
c = page_part(draw())
_mx = draw()
MIXER["on"] = False
lt = [r for r in c if r[0] == "lp_text"]
words = [r[3] for r in lt]
check("mixer page: the title bar names the parameter the knobs move (MIXER  FLTF) and the roll's rate", "MIXER  FLTF" in words and "ROLL 1/8" in words, words[:4])
check("... the first held track and its value shown (TRK 3  24)", "TRK 3  24" in words, words[:6])
fills = [r for r in c if r[0] == "lp_box" and r[3] == 10 and r[5] == 0x0F and TOP + 30 <= r[2] < TOP + 100]
check("... the two held tracks' faders filled", len(fills) == 2, fills)
check("... no 2x picture blitted (none of its 2-px runs)", len([r for r in c if r[0] == "lp_box" and r[4] == 2 and TOP + 14 <= r[2] < TOP + 128]) < 3, [r for r in c if r[0] == "lp_box" and r[4] == 2 and TOP + 14 <= r[2] < TOP + 128])
check("... the muted track shows M", "M" in words, words)
MIXER_PREVIEW = _mx


# --- the SAMPLE button's file player (abi 7): the ring onto a bus at 48 kHz, linear interpolation, added to what is there
LB, RB = 0x24077000, 0x24077800


def player(state, wr, rd, end, frac, step, gain=1.0):
    u.mem_write(PLAYER, struct.pack("<6If I", state, wr, rd, end, frac, step, gain, PRING))


def fl(addr, n):
    return list(struct.unpack("<%df" % n, bytes(u.mem_read(addr, 4 * n))))


ring = b"".join(struct.pack("<hh", 100 * k, -100 * k) for k in range(64))          # a ramp: left +100 a frame, right -100
u.mem_write(PRING, ring)
u.mem_write(LB, struct.pack("<16f", *([0.5] * 16)))
u.mem_write(RB, struct.pack("<16f", *([0.0] * 16)))
player(1, 64, 0, 64, 0, 65536 * 441 // 480)                       # a 44.1 kHz file: 0.91875 of its frames a 48 kHz frame
e.call("mcl_player_mix", LB, RB, 16)
lo, ro = fl(LB, 16), fl(RB, 16)
want = [0.5 + 100 * (k * 0.91875) / 32768 for k in range(16)]
check("player: a 44.1 kHz ramp is resampled to 48 kHz (linear) and added to the bus", all(abs(a - b) < 2e-4 for a, b in zip(lo, want)) and abs(ro[15] + 100 * 15 * 0.91875 / 32768) < 2e-4, (lo[:3], want[:3]))
st = struct.unpack("<6I", bytes(u.mem_read(PLAYER, 24)))
check("... the position moved on by 16 x 0.91875 frames (14.7)", st[2] == 14 and abs(st[4] / 65536 - 0.7) < 0.01, st)
player(1, 64, 60, 62, 0, 65536)
u.mem_write(LB, bytes(64))
e.call("mcl_player_mix", LB, RB, 16)
st = struct.unpack("<6I", bytes(u.mem_read(PLAYER, 24)))
check("... at the file's end it stops by itself (state 0)", st[0] == 0 and st[2] == 61, st)
player(1, 10, 9, 64, 0, 65536)
u.mem_write(LB, bytes(64))
e.call("mcl_player_mix", LB, RB, 16)
st = struct.unpack("<6I", bytes(u.mem_read(PLAYER, 24)))
check("... an empty ring (the card behind for a moment): silence, still playing", st[0] == 1 and st[2] == 9 and fl(LB, 16) == [0.0] * 16, st)
player(0, 64, 0, 64, 0, 65536)
e.call("mcl_player_active")
check("... mcl_player_active: 0 while stopped", u.reg_read(UC_ARM_REG_R0) == 0)

# --- the footer line: no buttons
check("taps on the footer line do nothing", touch(30, FOOT + 6) == ([], []) and touch(W // 2, FOOT + 6) == ([], []) and touch(W - 30, FOOT + 6) == ([], []))


# --- the preview picture (before the abi 4 / off states below)
def preview(path, c):
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return
    pal = {0x1C: (185, 188, 193), 0x1E: (43, 45, 49), 0x0E: (0, 0, 0), 0x0F: (240, 240, 240), 0x06: (229, 72, 77), 0x10: (90, 93, 100),
           0x1B: (124, 200, 240), 0x0B: (95, 207, 143), 0x09: (120, 125, 135), 0x1D: (210, 100, 44), 0x1F: (212, 42, 42)}
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
            if r[0] == "lp_text":
                x, d, s, col, sc = r[1:6]
                w = len(s) * 6 * sc - sc
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


state["page"] = 0
preview("out/mcl_page.png", draw())
state["page"] = 255
preview("out/mcl_grid.png", GRID_PREVIEW)
preview("out/mcl_mixer.png", MIXER_PREVIEW)

# --- an older blob (abi 4): the same panel, no shortcuts, no clock: the footer says what the knobs are
entry(4)
c = draw()
names = [r[-1] for r in c if r[0] == "lp_text_c"]
check("abi 4: no shortcuts", "CHROMATIC" not in names and "GRID" not in names and "NO" in names, names)
check("... taps where they would be do nothing", touch(W // 2, STRIP) == ([], []))

print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
