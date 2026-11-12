"""MCL screen and input (milestone M6) under Unicorn: src/mcl_bb/bb_ui.cpp (the 128x32 screen and the test bench behind it), the entry table's page functions,
and the cave's MCL page in src/cpu.c (scaled blit, tiles, knobs) with the looper page's drawing primitives replaced by recorders.

Checked: the screen text (decoded back from the frame buffer with the font), buttons FOLLOW / INT / STOP and the tempo encoder, tile and knob mapping, detent
accumulation, release of a tile on the right touch id, blit geometry (runs inside the frame, 2x), refusal while the runtime is down, signature changes.
NOT checked (hardware only): how the real page looks, touch feel, knob counts per detent.
"""
import struct
import sys

from mcl_rig import clock_rig
from unicorn.arm_const import UC_ARM_REG_R0

r = clock_rig()
e = r.e
src = open("src/mcl_bb/glcdfont.h").read()
body = src[src.index("{") + 1:src.rindex("}")]
FONT = [int(t, 16) for t in body.replace("//", " ").replace(",", " ").split() if t.startswith("0x")]
fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


def fb():
    p = r.call("bb_ui_fb")
    return bytes(e.uc.mem_read(p, 1024))


def px(f, x, y):
    return f[y * 16 + (x >> 3)] >> (7 - (x & 7)) & 1


def screen():
    r.call("bb_ui_refresh")
    r.call("bb_ui_sig")
    f = fb()
    rows = []
    for row in range(4):
        s = ""
        for c in range(21):
            cols = [sum(px(f, c * 6 + i, row * 8 + j) << j for j in range(7)) for i in range(5)]
            ch = next((chr(k) for k in range(32, 127) if [FONT[k * 5 + i] & 0x7f for i in range(5)] == cols), "?")
            s += ch
        rows.append(s)
    return rows


def entry(name, *a):
    e.call(name, *a)
    return e.uc.reg_read(UC_ARM_REG_R0)


# --- power-up: cpu.c start() clears the page state (backup SRAM keeps what the last run left)
e.uc.mem_write(0x38800930, b"\x5a" * 16)
r.boot()
check("start() clears the page's state in backup SRAM", bytes(e.uc.mem_read(0x38800930, 16)) == bytes(16))

# --- the screen before anything is pressed
rows = screen()
check("screen shows the title and IDLE", rows[0].startswith("MCL BB IDLE"), rows[0])
check("tempo defaults to 120.0", "BPM 120.0" in rows[1], rows[1])
sig0 = r.call("bb_ui_sig")
check("signature is stable while nothing changes", r.call("bb_ui_sig") == sig0)

# --- buttons and encoders through the blob entry (the cave's page does the same through the entry table)
r.call("bb_ui_input", 2, 1, 0)                                  # B2 down: our own clock
rows = screen()
check("B2 starts the internal clock", "INT" in rows[0] and "BPM 120.0" in rows[1], rows[0] + "|" + rows[1])
check("B2 pressed shows in the button bits", rows[3].endswith("B0100"), rows[3])
r.call("bb_ui_input", 3, 1, 0)
check("B2 released clears the bit", screen()[3].endswith("B0000"), screen()[3])
r.call("bb_ui_input", 1, 0, 3)                                  # encoder 1: +3 detents = +3 BPM
rows = screen()
check("encoder 1 raises the tempo 1 BPM a detent", "BPM 123.0" in rows[1], rows[1])
r.call("bb_ui_input", 2, 3, 0)                                  # B4 held: tenths
r.call("bb_ui_input", 1, 0, -2)
rows = screen()
check("B4 held: tenths", "BPM 122.8" in rows[1], rows[1])
check("encoder 1 value shown", rows[3].startswith("  1"), rows[3])
r.call("bb_ui_input", 3, 3, 0)
r.call("bb_ui_input", 2, 2, 0)                                  # B3: stop
rows = screen()
check("B3 stops", "IDLE" in rows[0], rows[0])
r.call("bb_ui_input", 3, 2, 0)
r.call("bb_ui_input", 2, 0, 0)                                  # B1: follow the jack
rows = screen()
check("B1 follows the jack (listening)", "FOLLOW" in rows[0] and "LISTEN" in rows[0], rows[0])
r.call("bb_ui_input", 3, 0, 0)
r.call("bb_ui_input", 1, 9, 1)                                  # bad indexes are ignored
r.call("bb_ui_input", 2, 9, 0)
check("bad encoder / button numbers are ignored", screen()[3].endswith("B0000"))

# --- the cave's page
rec = []
for name in ("lp_box", "lp_frame", "lp_text_c"):
    n = name
    e.stub(e.sym[n], lambda n=n: rec.append((n,) + tuple(e.arg(i) for i in range(4)) + ((e.cstr(e.arg(3)),) if n == "lp_text_c" else ())))
W, FOOT = 314, 224


def draw():
    rec.clear()
    e.call("mcl_page_draw", W, FOOT)
    return list(rec)


calls = draw()
boxes = [c for c in calls if c[0] == "lp_box"]
SW, SH, TOP = 128 * 12 // 5, 64 * 12 // 5, 19                    # the screen scaled by 2.4 (307 x 153)
sc = lambda v: v * 12 // 5
x0 = (W - SW) // 2
runs = [c for c in boxes if c[4] in (2, 3) and TOP <= c[2] < TOP + SH and x0 <= c[1] < x0 + SW]          # (name, x, d, w, h)
check("the screen is blitted in runs 2 or 3 px high (2.4x)", len(runs) > 20, len(runs))
check("runs stay inside the 307x153 area", all(c[1] + c[3] <= x0 + SW and c[2] + c[4] <= TOP + SH for c in runs))
f = fb()
lit = sum(px(f, x, y) for x in range(128) for y in range(64))
lit_w = {}
for y in range(64):
    for x in range(128):
        if px(f, x, y):
            lit_w[y] = lit_w.get(y, 0) + sc(x + 1) - sc(x)
got_w = {}
for c in runs:
    y = next(yy for yy in range(64) if TOP + sc(yy) == c[2])
    got_w[y] = got_w.get(y, 0) + c[3]
check("the runs cover exactly the lit pixels, each pixel 2 or 3 wide at x * 12 / 5", got_w == lit_w, (sum(got_w.values()), sum(lit_w.values())))
check("tile frames are drawn (screen frame + 5 tiles)", len([c for c in calls if c[0] == "lp_frame"]) == 6)
check("tile names and roles are drawn", len([c for c in calls if c[0] == "lp_text_c"]) == 10)
texts = [e.cstr(c[4]) for c in calls if c[0] == "lp_text_c"]
import re
check("under B2: the MIDI jack's speed and bytes received ('1X <n>'); under B3: receive errors ('0 ERR')",
      any(re.fullmatch(r"1X \d+", t) for t in texts) and "0 ERR" in texts, texts)

# --- the crash recorder's record of an unexpected restart: shown over MCL's screen, a tap on the screen clears it
REC = 0x38800B38
crec = struct.pack("<IHHI", 0x31435243, 3, 1, 0) + bytes([1, 2, 1, 0, 0, 0]) + struct.pack("<5I", 0x08100001, 0x08100003, 0x01000000, 0x2401F000, 0xFFFFFFF9)
e.uc.mem_write(REC, crec + bytes(0x60))
calls = draw()
texts = [c[5] for c in calls if c[0] == "lp_text_c"]
check("a restart record shows over MCL's screen (its first line, the hint to clear it)", any(t.startswith("LAST RESTART") for t in texts) and "TAP HERE TO CLEAR" in texts, texts[:4])
entry("mcl_page_down", 3, x0 + 40, TOP + 40, W, FOOT)
calls = draw()
texts = [c[5] for c in calls if c[0] == "lp_text_c"]
check("a tap on the screen clears it (gone from the page)", not any(t.startswith("LAST RESTART") for t in texts) and "TAP HERE TO CLEAR" not in texts, texts[:4])
entry("mcl_page_up", 3)

crec2 = struct.pack("<IHHI", 0x31435243, 4, 1, 1 << 24) + bytes([0, 2, 0, 0, 0, 0])      # a software reset: no fault, no watchdog
e.uc.mem_write(REC, crec2 + bytes(0x60))
calls = draw()
texts = [c[5] for c in calls if c[0] == "lp_text_c"]
check("a software reset (no fault, no watchdog) also shows its record", any(t.startswith("LAST RESTART") for t in texts), texts[:3])
e.uc.mem_write(REC, struct.pack("<IHHI", 0x31435243, 5, 1, 1 << 23) + bytes([0, 2, 0, 0, 0, 0]) + bytes(0x60))
calls = draw()
texts = [c[5] for c in calls if c[0] == "lp_text_c"]
check("... a plain power-on does not", not any(t.startswith("LAST RESTART") for t in texts), texts[:3])

sig_a = entry("mcl_page_sig")
check("page signature depends on the picture", sig_a == entry("mcl_page_sig"))
# a tile touch: x in tile 0, d in the tile row
TILE_Y = TOP + SH + 5
tw = (W - 8) // 5
entry("mcl_page_down", 1, 4 + 2 * tw + 5, TILE_Y + 5, W, FOOT)    # touch id 1 on B3 (stop)
check("a tile touch presses B3 (stop) and shows it pressed", screen()[3].endswith("B0010"), screen()[3])
sig_b = entry("mcl_page_sig")
check("the signature changes while a tile is held", sig_a != sig_b)
entry("mcl_page_up", 1)
check("the release returns the signature", entry("mcl_page_sig") != sig_b)
entry("mcl_page_down", 0, 4 + 5, 40, W, FOOT)                    # on the screen area: ignored
check("touching the screen does nothing", screen()[3].endswith("B0000"))
entry("mcl_page_down", 2, 4 + 0 * tw + 5, TILE_Y + 5, W, FOOT)    # B1 down on touch id 2
check("B1 tile: follow", "FOLLOW" in screen()[0], screen()[0])
check("the tile shows in the button bits", screen()[3].endswith("B1000"), screen()[3])
entry("mcl_page_up", 2)
entry("mcl_page_up", 3)
check("button released", screen()[3].endswith("B0000"), screen()[3])

# --- knobs: 40 counts a step, remainder kept; a quick spin (80 counts or more in one event) doubles the steps
e0 = screen()[3]
entry("mcl_page_knob", 2, 30)
check("under one step: nothing", screen()[3] == e0)
entry("mcl_page_knob", 2, 30)
check("the remainder counts: one step (20 left)", screen()[3] != e0 and screen()[3][8:11] == "  1", screen()[3])
entry("mcl_page_knob", 2, -60)
check("negative counts turn the other way (-40: one step back)", screen()[3][8:11] == "  0", screen()[3])
entry("mcl_page_knob", 2, -100)
check("a quick spin (-100 in one event): two steps, doubled", screen()[3][8:11] == " -4", screen()[3])
entry("mcl_page_knob", 7, 100)
entry("mcl_page_knob", -1, 100)
check("bad knob numbers are ignored", screen()[3][8:11] == " -4")

# --- a boot after a reset that was not a power-on (cpu_boot left M->magic = WARM): MCL is not started, its page says so; a tap starts it
boots, guis = [], []
e.stub(r.b["mcl_boot"], lambda: boots.append(1))
e.stub(r.b["bb_rt_gui"], lambda: guis.append(1))
e.uc.mem_write(REC, struct.pack("<IHHI", 0x31435243, 6, 1, (1 << 24) | (1 << 22)) + bytes([0, 2, 0, 0, 0, 0]) + bytes(0x60))
e.w32(0x2405FFD0, 0x4D524157)
r.boot()
check("warm boot: start() runs, MCL is not booted, the page state says off, no GUI call into the blob", boots == [] and guis == [] and e.r8(0x3880093E) == 1, (boots, guis, e.r8(0x3880093E)))
calls = draw()
texts = [c[5] for c in calls if c[0] == "lp_text_c"]
check("... the page: 'MCL IS OFF', the restart's reason, 'TAP HERE TO START MCL'; no picture from the last run",
      "MCL IS OFF: THE BLACKBOX RESTARTED" in texts and any(t.startswith("LAST RESTART") for t in texts) and "TAP HERE TO START MCL" in texts
      and not [c for c in calls if c[0] == "lp_box" and c[4] in (2, 3)], texts[:4])
sig_off = entry("mcl_page_sig")
entry("mcl_page_down", 1, 4 + 2 * tw + 5, TILE_Y + 5, W, FOOT)
entry("mcl_page_knob", 2, 100)
check("... tiles and knobs do nothing while it is off", boots == [] and e.r16(0x38800934) == 0, e.r16(0x38800934))
entry("mcl_page_up", 1)
entry("mcl_page_down", 3, x0 + 40, TOP + 40, W, FOOT)
check("a tap on the screen: MCL booted and its task started once, page state on, record seen",
      boots == [1] and guis == [1] and e.r8(0x3880093E) == 0 and e.r8(REC + 13) == 3, (boots, guis, e.r8(0x3880093E), e.r8(REC + 13)))
check("... the page signature changed", entry("mcl_page_sig") != sig_off)
e.w32(0x2405FFD0, 0)
r.boot()
check("power-on boot (magic 0): MCL booted", boots == [1, 1] and e.r8(0x3880093E) == 0, boots)

print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
