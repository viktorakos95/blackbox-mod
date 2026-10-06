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
boot()
SOLO, VIEW, APP, PTA, CTX = 0x2405FF60, 0x24040000, 0x24030000, 0x24038000, 0x24039000
PAGE = 0x38800F00
e.uc.mem_write(VIEW, bytes(0x2000))
e.uc.mem_write(SOLO, struct.pack("<IBBBBIHH", 0x534F4C4F, 1, 0, 0, 0, VIEW, 0, 0))   # in Solo mode
for i in range(16):
    c = VIEW + 0x3AC + i * 0x1A0
    e.uc.mem_write(c + 0x38, struct.pack("<H", (i // 4) << 4 | (i % 4)))
    e.uc.mem_write(c + 4, struct.pack("<4i", 120 * (i % 4), 60 * (i // 4), 120, 60))
e.uc.mem_write(VIEW + 0x1E40, b"\x01")
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
hit = {"cell": 0}


def fake_hit():
    e.w32(e.arg(2), hit["cell"])
    e.ret(1)


e.stubs[0x080B5E78] = fake_hit


def touch(kind, row, col, x=None, y=None):
    hit["cell"] = VIEW + 0x3AC + (row * 4 + col) * 0x1A0
    e.uc.mem_write(PTA, struct.pack("<2i", 120 * col + 10 if x is None else x, 60 * row + 10 if y is None else y))
    e.call({"down": "solo_touch_down", "move": "solo_touch_move", "up": "solo_touch_up"}[kind], VIEW, PTA, 0)


def evs(t):
    return list(e.uc.mem_read(STATE + T0 + TSIZE * t + 8, 5))


touch("down", 0, 2)
touch("up", 0, 2)
check("box: down and up become REC_DOWN / REC_UP for that track", evs(2)[:2] == [1, 1], evs(2))
touch("down", 1, 1, x=120 + 5)
check("row 1, left third: PAN selected for track 2 (knob turns pan)", e.r8(PAGE + 6) == 0b10, e.r8(PAGE + 6))
touch("up", 1, 1)
touch("down", 1, 1, x=120 + 60)
touch("up", 1, 1)
check("row 1, middle third: REVERSE", evs(1)[4] == 1, evs(1))
touch("down", 1, 1, x=120 + 110)
touch("up", 1, 1, x=120 + 110)
check("row 1, right third: M down / up", evs(1)[2:4] == [1, 1], evs(1))
touch("down", 3, 0, y=180 + 60 - 6)
lv = tr(0, 0x20, "f")
check("fader: touching its bottom sets level 0", abs(lv) < 1e-6, lv)
touch("move", 2, 0, y=120 + 6)
lv = tr(0, 0x20, "f")
check("fader: dragging to its top sets level 1", abs(lv - 1.0) < 1e-6, lv)
touch("up", 2, 0)
check("the page's touches never reach the stock mixer handlers", stock == [], stock)

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
knob(1, 1600)
check("knob 2 turns track 2's pan while PAN is selected", abs(tr(1, 0x24, "f") - 0.4) < 1e-5, tr(1, 0x24, "f"))
knob(0, 100, msg_id=0x63)
check("other view messages go to the stock handler", viewmsg == [0x63], viewmsg)
e.uc.mem_write(SOLO + 6, b"\x00")
knob(0, 800)
check("off the page, knobs go to the stock handler too", viewmsg == [0x63, 0x32] and abs(tr(0, 0x20, "f") - 0.4) < 1e-5, viewmsg)
e.uc.mem_write(SOLO + 6, b"\x01")

# app messages: the last hardware button is noted
appmsg = []
e.stub(0x080A2E60, lambda: appmsg.append(e.r16(e.arg(1))))
e.uc.mem_write(MSG, struct.pack("<H", 0xF9) + bytes(10) + struct.pack("<i", 6))
e.call("looper_app_msg", APP, MSG)
check("app message passed on, button noted (f9:6)", appmsg == [0xF9] and e.r16(PAGE + 8) == 0xF9 and e.r16(PAGE + 10) == 6, (appmsg, e.r16(PAGE + 8)))

# drawing
fills, texts = [], []
e.stub(0x0808EA22, lambda: fills.append(e.arg(1)))
e.stub(0x0808E994)
e.stub(0x0808EE54, lambda: texts.append(bytes(e.uc.mem_read(e.arg(0), 12)).split(b"\0")[0].decode()))
e.stub(0x0808ED28, value=20)
celldraw = []
e.stub(0x080A43B8, lambda: celldraw.append(e.arg(0)))
e.uc.mem_write(CTX, b"\x01\x00\x00\x00" + struct.pack("<I", 0x24039100))


def draw(row, col):
    fills.clear()
    texts.clear()
    celldraw.clear()
    e.call("looper_cell_draw", VIEW + 0x3AC + (row * 4 + col) * 0x1A0, CTX)
    return list(fills), list(texts)


f, t = draw(0, 0)
check("box of an empty track: dark, labelled '1' / 'EMPTY'", f[0] == 0x19 and t[:2] == ["1", "EMPTY"], (f, t))
ev(0, REC_DOWN)
block([0.2] * N)
f, t = draw(0, 0)
check("while recording: red, 'REC'", f[0] == 0x06 and "REC" in t, (f, t))
f, t = draw(1, 1)
check("row 1: PAN (selected: teal), REV, M", f[0] == 0x1A and t == ["PAN", "REV", "M"], (f, t))
f, t = draw(2, 0)
check("fader top half: shows the level in %", "40" in t, t)
f, t = draw(3, 3)
check("bottom-right corner shows the last button message", "bf9:6" in t, t)
e.uc.mem_write(SOLO + 6, b"\x00")
draw(0, 0)
check("off the page the stock cell draw runs", celldraw == [VIEW + 0x3AC] and not fills)

print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
