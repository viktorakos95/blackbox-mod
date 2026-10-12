"""The TRIG page (src/t2m_page.c inside src/looper_page.c, third page of the SONG button) under Unicorn, on the patched image.

Uses test_looper.py's setup (the fake pool, the mixer view with the real 256 x 224 grid, the drawing stubs) by running
that file up to its first page draw, then: the SONG button's three pages, the footer's sheets, the page's switches, rows
and knobs, INT, the redraw signature, and a PNG of each sheet (out/t2m_test/trig_*.png, colours from the firmware's palette).

python3 patch.py solo slice duck chord cond filter cpu od comp seqfix fx2 munchi looper cave && python3 test_t2m_page.py
"""
import os
import struct
import subprocess
import sys
import zlib

src = open("test_looper.py").read()
exec(compile(src[:src.index("f = draw(5)")], "test_looper.py (setup)", "exec"))   # noqa: S102 - our own test file

import unicorn.arm_const as A  # noqa: E402

fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


FIELDS = ["on", "port", "last_vel", "last_note", "hits", "peak", "blocks", "hit_blk", "looped", "looped_lost", "hist_n", "hist", "ui", "p",
          "clk"]
os.makedirs("out/t2m_test", exist_ok=True)
open("out/t2m_test/off2.c", "w").write(
    '#include <stdio.h>\n#include <stddef.h>\n#include "../../src/t2m_bb.h"\nint main(void){\n'
    + "".join(f'printf("{f} %zu\\n", offsetof(struct t2m_mem, {f}));\n' for f in FIELDS)
    + 'printf("PSIZE %zu\\n", sizeof(t2m_params));\n'
    + "".join(f'printf("p_{f} %zu\\n", offsetof(t2m_params, {f}));\n' for f in
            ("sens", "mask_ms", "floor_db", "scan_ms", "note", "note_on", "vel_mode", "vel_fixed", "cc_on", "cc_num", "len_mode", "len_ms"))
    + "}\n")
subprocess.run(["gcc", "-o", "out/t2m_test/off2", "out/t2m_test/off2.c"], check=True)
O = {l.split()[0]: int(l.split()[1]) for l in
     subprocess.run(["out/t2m_test/off2"], capture_output=True, text=True, check=True).stdout.splitlines()}
PSIZE = O["PSIZE"]

# memory for Trigger2MIDI: the allocator stubbed (the looper test's boot ran with the real one: no memory)
MEM, ENGINE_PAD = 0x30030000, 0x2400A9C0
e.stub(0x080443F0, fn=lambda: e.uc.reg_write(A.UC_ARM_REG_R0, 3 << 20))
e.stub(0x08044404, fn=lambda: e.uc.reg_write(A.UC_ARM_REG_R0, MEM))
e.call("t2m_boot")
check("Trigger2MIDI has its memory", e.r32(0x38800984) == MEM)


def p8(k, off):
    return e.r8(MEM + O["p"] + k * PSIZE + off)


def touch(kind, x_rel, d):
    """As test_looper.py: fill-space point x = 3 + x_rel, y = 224 - d."""
    e.uc.mem_write(PTA, struct.pack("<2i", 3 + x_rel, 224 - d))
    e.call({"down": "solo_touch_down", "move": "solo_touch_move", "up": "solo_touch_up"}[kind], VIEW, PTA, 0)


def tap(x, d):
    touch("down", x, d)
    touch("up", x, d)


MSG = 0x2403A000


def knob(i, counts):
    """As test_looper.py: knob i (track i's), counts; the left encoders arrive swapped."""
    i = i ^ 1 if i < 2 else i
    e.uc.mem_write(MSG, struct.pack("<H", 0x32) + bytes(10) + struct.pack("<h", i) + bytes(2) + struct.pack("<h", counts))
    e.call("looper_view_msg", VIEW, MSG)


def sig():
    e.call("t2m_page_sig")
    return e.uc.reg_read(A.UC_ARM_REG_R0)


# ---- the SONG button: SAMPLR, TRIG, the stock song screen
e.uc.mem_write(APP + 0x8CA4, b"\x2f")
screens.clear()
e.call("solo_song_pressed", APP, 0, 0, 0)
check("SONG: the SAMPLR page first", screens == [] and e.r8(PAGE + 76) == 4, (screens, e.r8(PAGE + 76)))
e.call("solo_song_pressed", APP, 0, 0, 0)
check("SONG again: the TRIG page, no screen change", screens == [] and e.r8(PAGE + 76) == 5, (screens, e.r8(PAGE + 76)))
e.call("looper_page_tab")
check("looper_page_tab reports TRIG as 6", e.uc.reg_read(A.UC_ARM_REG_R0) == 6)
e.call("solo_mix_pressed", APP, 0, 0, 0)
check("MIX on the TRIG page goes to the Looper's MAIN tab", screens == [] and e.r8(PAGE + 76) == 0, (screens, e.r8(PAGE + 76)))
e.call("looper_page_goto", VIEW, 5)
e.call("solo_song_pressed", APP, 0, 0, 0)
check("SONG from SAMPLR again TRIG, then once more the stock song screen (0x2d)", e.r8(PAGE + 76) == 5, e.r8(PAGE + 76))
e.call("solo_song_pressed", APP, 0, 0, 0)
check("... the stock song screen", screens == [0x2D], screens)

e.call("looper_page_goto", VIEW, 6)
check("the TRIG page is up (mode 5)", e.r8(PAGE + 76) == 5)


# ---- a picture of the screen
fw = open("firmware/BLACKBOX-3.1.9.bin", "rb").read()
pal = [struct.unpack_from("<I", fw, 0x080F1D80 - 0x08040000 + 4 * i)[0] for i in range(48)]
W, H, S = 320, 224, 3


def png(path, w, h, data):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(data, 9)) + chunk(b"IEND", b""))


def picture(name):
    f = draw(0)
    img = bytearray(b"\x00" * (W * H * 3))

    def put(x, y, c):
        if 0 <= x < W and 0 <= y < H:
            a = pal[c] if c < len(pal) else 0xFFFF00FF
            o = ((H - 1 - y) * W + x) * 3                    # fill space is y up
            img[o:o + 3] = bytes(((a >> 16) & 255, (a >> 8) & 255, a & 255))
    for x, y, w, h, c, _ in f:
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                put(xx, yy, c)
    for x, y, c in pixels:
        put(x, y, c)
    big = b"".join(b"\x00" + b"".join(bytes(img[(r // S * W + c // S) * 3:(r // S * W + c // S) * 3 + 3]) for c in range(W * S))
                   for r in range(H * S))
    png(f"out/t2m_test/trig_{name}.png", W * S, H * S, big)
    return f


def inside(f):
    return (all(x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224 for x in f) and
            all(3 <= px < 317 and 0 <= py < 224 for px, py, _ in pixels))


# ---- footer: three sheets
f = draw(0)
check("TRIG page: no looper tab strip, no looper track columns", not any(x[2] == 71 for x in f) and not any(x[2] == 34 and x[3] == 1 for x in f))
check("footer: the four sheets (HITS, DETECT, OUT, CLK), 58 px wide at 60 px pitch",
      all(any(x[0] == 3 + 3 + 60 * i and x[2] == 58 and x[3] == 1 for x in f) for i in range(4)), [x for x in f if x[3] == 1 and x[2] == 58][:4])
check("TRIG page: inside the screen, text drawn", inside(f) and len(pixels) > 1500, len(pixels))
check("HITS is the first sheet", e.r8(MEM + O["ui"] + 2) == 0)

# ---- the HITS sheet: pads, meters, history
# input L: five hits over the last 3.7 s (ages in audio blocks of 5.33 ms), the last one 5 blocks ago; input R: two
e.w32(MEM + O["blocks"], 100000)
for k, hits in ((0, [(700, 40, 38), (520, 70, 38), (360, 100, 38), (200, 127, 38), (5, 90, 38)]), (1, [(600, 55, 45), (150, 120, 45)])):
    for i, (age, vel, note) in enumerate(hits):
        e.uc.mem_write(MEM + O["hist"] + (k * 24 + i) * 8, struct.pack("<IBBxx", 100000 - age, vel, note))
    e.w32(MEM + O["hits"] + 4 * k, 120 + len(hits))
    e.w32(MEM + O["hit_blk"] + 4 * k, 100000 - hits[-1][0])
    e.uc.mem_write(MEM + O["last_vel"] + k, bytes([hits[-1][1]]))
    e.uc.mem_write(MEM + O["last_note"] + k, bytes([hits[-1][2]]))
e.uc.mem_write(MEM + O["on"] + 1, b"\x01")
e.uc.mem_write(MEM + O["peak"], struct.pack("<ff", 0.35, 0.02))
f = picture("hits")
check("HITS sheet: inside the screen", inside(f))
check("HITS sheet: input L's pad is lit in the colour of velocity 90 (yellow, 0x14), R's is not",
      any(x[4] == 0x14 and x[2] > 100 and x[3] > 40 for x in f) and not any(x[4] in (0x0B, 0x06) and x[2] > 100 and x[3] > 40 for x in f))
check("HITS sheet: the seven hits of the last 4 s are bars (3 px wide), taller for harder hits",
      sum(1 for x in f if x[2] == 3 and x[4] in (0x1B, 0x14, 0x0F)) == 7 and
      sorted(x[3] for x in f if x[2] == 3 and x[4] == 0x1B)[0] < sorted(x[3] for x in f if x[2] == 3 and x[4] == 0x1B)[-1],
      [(x[3], x[4]) for x in f if x[2] == 3])
check("HITS sheet: the newest hit's bar is white (still lit)", any(x[2] == 3 and x[4] == 0x0F for x in f))
tap(200, 60)
check("HITS: a tap on R's pad shows input R", e.r8(MEM + O["ui"]) == 1)
tap(60, 26)
check("IN R / ... the switch row works on this sheet", e.r8(MEM + O["ui"]) == 1)
tap(20, 26)
knob(0, 40 * 20)
sens = struct.unpack("<f", e.uc.mem_read(MEM + O["p"], 4))[0]
check(f"HITS: knob 1 turns SENS of the input shown: 2.00 -> {sens:.2f}", abs(sens - 4.0) < 1e-4, sens)
knob(1, 40 * 4)
thr = struct.unpack("<f", e.uc.mem_read(MEM + O["p"] + 4, 4))[0]
check(f"HITS: knob 2 turns THRESH: 2.00 -> {thr:.2f}", abs(thr - 3.0) < 1e-4, thr)
s_now = sig()
e.w32(MEM + O["blocks"], 100016)
check("HITS: the page redraws as the history scrolls while it holds a recent hit", sig() != s_now)
e.w32(MEM + O["blocks"], 200000)
s_now = sig()
e.w32(MEM + O["blocks"], 200016)
check("HITS: ... and stops redrawing once the last 4 s are empty", sig() == s_now)

# ---- the sheets of the footer
tap(3 + 60 + 10, 216)
check("footer: DETECT", e.r8(MEM + O["ui"] + 2) == 1)
f = picture("detect")
check("DETECT sheet: inside the screen", inside(f))
fl = lambda off: struct.unpack("<f", e.uc.mem_read(MEM + O["p"] + off, 4))[0]
tap(150, 54 + 10)                                          # DETECT row 0: SENS THRESH RETRIG MASK
knob(3, -40 * 30)
check(f"DETECT: MASK goes down to 1 ms (the patch's range stopped at 15): {fl(O['p_mask_ms']):.0f} ms", fl(O["p_mask_ms"]) == 1.0, fl(O["p_mask_ms"]))
knob(3, 40 * 14)
check("... one ms a step: back to 15", fl(O["p_mask_ms"]) == 15.0, fl(O["p_mask_ms"]))
tap(150, 134 + 10)                                         # DETECT row 2: BLEED RANGE FLOOR
knob(2, -40 * 40)
check(f"DETECT: FLOOR -60 dB down 40 steps: {fl(O['p_floor_db']):.0f} dB", fl(O["p_floor_db"]) == -100.0, fl(O["p_floor_db"]))
knob(2, -40 * 60)
check("... stops at -120 dB", fl(O["p_floor_db"]) == -120.0)
f = picture("detect_floor")
check("... and the cell shows it", inside(f))
knob(2, 40 * 120)
check("FLOOR up stops at -40 dB", fl(O["p_floor_db"]) == -40.0)
knob(2, -40 * 20)
check("... back to the patch's -60 dB", fl(O["p_floor_db"]) == -60.0, fl(O["p_floor_db"]))
touch("down", 3 + 120 + 10, 216)
touch("up", 3 + 120 + 10, 216)
check("footer: OUT", e.r8(MEM + O["ui"] + 2) == 2)
f = picture("out")
check("OUT sheet: inside the screen", inside(f))


# ---- switches (row at d 18 .. 34): IN L 4..42, IN R 46..84, ON 104..144, TRS 162..200, USB 204..242
tap(60, 26)
check("IN R: the page shows input R", e.r8(MEM + O["ui"]) == 1)
tap(120, 26)
check("ON: switches input R off (it was on)", e.r8(MEM + O["on"] + 1) == 0)
tap(120, 26)
tap(180, 26)
check("TRS: input R's TRS output off (USB stays)", e.r8(MEM + O["port"] + 1) == 2, e.r8(MEM + O["port"] + 1))
tap(180, 26)
tap(20, 26)
check("IN L: back to input L", e.r8(MEM + O["ui"]) == 0)

# ---- OUT rows (d 40, 80; 40 px each): NOTE VEL LEN CHAN / CC CC VAL
tap(150, 40 + 10)
check("tapping the NOTE row hands it to the knobs", e.r8(MEM + O["ui"] + 1) == 0)
knob(0, 40)
check("knob 1, one step up: NOTE 38 -> 39", p8(0, O["p_note"]) == 39, p8(0, O["p_note"]))
knob(0, -40 * 60)
check("down past 0: NOTE OFF", p8(0, O["p_note_on"]) == 0)
knob(0, 40 * 39)
check("back up: note on again at 38", p8(0, O["p_note_on"]) == 1 and p8(0, O["p_note"]) == 38)
knob(1, 40 * 100)
check("knob 2: VEL from DYN to fixed 100", p8(0, O["p_vel_mode"]) == 1 and p8(0, O["p_vel_fixed"]) == 100)
scan = lambda: struct.unpack("<f", e.uc.mem_read(MEM + O["p"] + O["p_scan_ms"], 4))[0]
check("VEL fixed: SCAN dropped to 0 (no peak search needed)", scan() == 0.0, scan())
knob(1, -40 * 200)
check("... and back down to DYN", p8(0, O["p_vel_mode"]) == 0)
check("... SCAN is back to what it was (2.0 ms)", abs(scan() - 2.0) < 1e-6, scan())
knob(1, 40 * 5)
e.uc.mem_write(MEM + O["p"] + O["p_scan_ms"], struct.pack("<f", 1.5))
knob(1, -40 * 10)
check("SCAN changed by hand while VEL was fixed stays as set when VEL goes back to DYN", abs(scan() - 1.5) < 1e-6, scan())
knob(1, 40 * 5)
check("VEL fixed again: SCAN 0, the 1.5 ms remembered", scan() == 0.0)
e.uc.mem_write(MEM + O["p"] + O["p_scan_ms"], struct.pack("<f", 0.0))
knob(1, -40 * 10)
check("... and back to DYN: the remembered 1.5 ms", abs(scan() - 1.5) < 1e-6, scan())
e.uc.mem_write(MEM + O["p"] + O["p_scan_ms"], struct.pack("<f", 2.0))
knob(2, 40 * 3)
check("knob 3: LEN from DYN: the first step lands on 5 ms", p8(0, O["p_len_mode"]) == 1 and e.r16(MEM + O["p"] + O["p_len_ms"]) == 5, e.r16(MEM + O["p"] + O["p_len_ms"]))
knob(2, 40 * 2)
check("... then 5 ms a step: 15 ms", e.r16(MEM + O["p"] + O["p_len_ms"]) == 15, e.r16(MEM + O["p"] + O["p_len_ms"]))
knob(2, -40 * 5)
check("... and back to DYN", p8(0, O["p_len_mode"]) == 0)
tap(150, 80 + 10)
knob(0, 40 * 75)
check("CC row, knob 1: CC OFF -> CC 74", p8(0, O["p_cc_on"]) == 1 and p8(0, O["p_cc_num"]) == 74)
knob(0, -40 * 200)
check("... and OFF again", p8(0, O["p_cc_on"]) == 0)

# INT: the Blackbox's own MIDI in (switch row, sixth button at x 234 .. 272)
check("INT starts off", e.r8(MEM + O["port"]) == 3, e.r8(MEM + O["port"]))
tap(250, 26)
check("INT: a tap switches the Blackbox's own MIDI in on for the input shown (TRS and USB stay on)", e.r8(MEM + O["port"]) == 7, e.r8(MEM + O["port"]))
tap(170, 26)
tap(210, 26)
check("... TRS and USB off: INT only", e.r8(MEM + O["port"]) == 4, e.r8(MEM + O["port"]))
e.w32(MEM + O["looped"], 12)
f = picture("out_int")
check("OUT sheet with INT on: inside the screen, the sent counter and the explanation are drawn", inside(f))
tap(250, 26)
tap(170, 26)
tap(210, 26)
check("... INT off, the jack and USB back on", e.r8(MEM + O["port"]) == 3, e.r8(MEM + O["port"]))
tap(60, 26)
tap(250, 26)
check("input R has its own port byte", e.r8(MEM + O["port"] + 1) == 7 and e.r8(MEM + O["port"]) == 3, (e.r8(MEM + O["port"]), e.r8(MEM + O["port"] + 1)))
tap(250, 26)
tap(20, 26)
check("the input R settings did not move", p8(1, O["p_note"]) == 38 and p8(1, O["p_cc_on"]) == 0)
check("knobs on the TRIG page never move the looper's faders", abs(tr(0, O_LEVEL, "f") - 1.0) < 1e-6, tr(0, O_LEVEL, "f"))

# ---- the signature follows what the page shows
s0 = sig()
e.w32(MEM + O["hits"], 5)
check("the page redraws when a hit is counted", sig() != s0)
s0 = sig()
e.w32(MEM + O["looped"], 99)
check("... and when a note is put into the Blackbox", sig() != s0)

# ---- the CLK sheet (src/clkmon.c): a measurement of the incoming MIDI clock, a fourth sheet of the footer
import random  # noqa: E402

CYC, MGR = 0xE0001004, 0x24002E88
e.w32(0xE000E014, 400_000 - 1)                              # SysTick reload: a 400 MHz core
e.w32(0x580244E0, 1 << 28)                                # RCC: backup SRAM clock
e.w32(0x58024800, 1 << 8)                                 # PWR: backup domain writes
e.uc.mem_write(MGR + 0x20D, b"\x01")
tap(3 + 180 + 10, 216)
check("footer: CLK", e.r8(MEM + O["ui"] + 2) == 3, e.r8(MEM + O["ui"] + 2))
rng = random.Random(5)
blk, tick = 2133333, 8333333
arr = [300000 + i * tick + rng.randint(-900, 900) for i in range(60)]
ev = sorted([(t, 1) for t in arr] + [(blk * b + 777, 0) for b in range(1, arr[-1] // blk + 3)])
for t, is_tick in ev:
    e.w32(CYC, t)
    if is_tick:
        e.call("clkmon_rx", MGR, 0xF8, 0)
    else:
        e.call("clkmon_block", MEM + O["clk"], 128)
e.w32(CYC, 2_100_000_000)
e.call("clkmon_rx", MGR, 0xFA, 0)
f = picture("clk")
check("CLK sheet: inside the screen, text drawn", inside(f) and len(pixels) > 800, len(pixels))
check("CLK sheet: no switch row (nothing at the TRIG switch row's buttons)", not any(x[0] == 4 and x[1] == 18 and x[2] == 38 for x in f))
s0 = sig()
e.w32(CYC, 2_100_001_000)
e.call("clkmon_rx", MGR, 0xF8, 0)
check("CLK sheet: the page redraws when a tick arrives", sig() != s0)
n0 = p8(0, O["p_note"])
knob(0, 40 * 20)
check("knobs do nothing on the CLK sheet", p8(0, O["p_note"]) == n0 and e.r8(MEM + O["ui"] + 2) == 3)
tap(100, 100)
e.call("clkmon_block", MEM + O["clk"], 128)
e.call("clkmon_stats", MEM + O["clk"], 0, 0x30038000)
check("a tap on the sheet resets the numbers (counted from zero after the next block)",
      e.r32(0x30038000) == 0 and e.r32(0x30038014) == 0, (e.r32(0x30038000), e.r32(0x30038014)))
tap(3 + 60 + 10, 216)
check("back to DETECT from CLK", e.r8(MEM + O["ui"] + 2) == 1)
f = picture("detect_after_clk")
check("DETECT after CLK: inside the screen", inside(f))

print("ALL PASS" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
