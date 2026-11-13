"""MCL screen and input (milestone M6) under Unicorn: src/mcl_bb/bb_ui.cpp (the 128x32 screen and the test bench behind it), the entry table's page functions,
and the cave's MCL page in src/cpu.c (2x blit, buttons, knobs) with the looper page's drawing primitives replaced by recorders (its layout: test_mcl_page.py).

Checked: the screen text (decoded back from the frame buffer with the font), buttons FOLLOW / INT / STOP and the tempo encoder, button and knob mapping, detent
accumulation, release of a button on the right touch id, blit geometry (2x), no restart record over the screen, refusal while the runtime is down, signature changes.
NOT checked (hardware only): how the real page looks, touch feel, knob counts per detent.
"""
import struct
import sys

from mcl_rig import clock_rig
from unicorn.arm_const import UC_ARM_REG_R0

from emu import STACK

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
e.uc.mem_write(0x38800680, b"\x5a" * 16)
r.boot()
check("start() clears the page's state in backup SRAM", bytes(e.uc.mem_read(0x38800680, 16)) == bytes(16))

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

# --- the cave's page (its layout in detail: test_mcl_page.py, which needs no stock image)
rec = []
for name in ("lp_box", "lp_frame", "lp_text_c"):
    n = name
    e.stub(e.sym[n], lambda n=n: rec.append((n,) + tuple(e.arg(i) for i in range(4)) + ((e.cstr(e.arg(3)),) if n == "lp_text_c" else ())))
W, FOOT = 314, 224


def draw():
    rec.clear()
    e.call("mcl_page_draw", W, FOOT)
    return list(rec)


def page_down(tid, x, d):
    e.w32(STACK, FOOT)                                           # the fifth argument (foot_y) is on the stack: e.call starts with SP = STACK
    e.call("mcl_page_down", tid, x, d, W)


calls = draw()
boxes = [c for c in calls if c[0] == "lp_box"]
SW, SH, TOP = 256, 128, 36                                       # the screen at 2x, in the MegaCommand-style window
x0 = (W - SW) // 2
runs = [c for c in boxes if c[4] == 2 and TOP <= c[2] < TOP + SH and x0 <= c[1] < x0 + SW]          # (name, x, d, w, h)
check("the screen is blitted in runs 2 px high (2x)", len(runs) > 20, len(runs))
f = fb()
lit_w = {}
for y in range(64):
    for x in range(128):
        if px(f, x, y):
            lit_w[y] = lit_w.get(y, 0) + 2
got_w = {}
for c in runs:
    y = (c[2] - TOP) // 2
    got_w[y] = got_w.get(y, 0) + c[3]
check("the runs cover exactly the lit pixels, each pixel 2 wide", got_w == lit_w, (sum(got_w.values()), sum(lit_w.values())))
texts = [c[5] for c in calls if c[0] == "lp_text_c"]
check("the panel: NO / PAGE / YES / SHIFT (the mark and MACHINEDRUM are drawn pixel by pixel: test_mcl_page.py)", all(t in texts for t in ("NO", "PAGE", "YES", "SHIFT")), texts)
check("no MIDI counters on the page", not any("ERR" in t for t in texts), texts)

# --- the crash recorder's record of an unexpected restart: kept (src/crash.c), no longer shown over MCL's screen (docs/diagnostics.md)
REC = 0x38800B38
crec = struct.pack("<IHHI", 0x31435243, 3, 1, 0) + bytes([1, 2, 1, 0, 0, 0]) + struct.pack("<5I", 0x08100001, 0x08100003, 0x01000000, 0x2401F000, 0xFFFFFFF9)
e.uc.mem_write(REC, crec + bytes(0x60))
calls = draw()
texts = [c[5] for c in calls if c[0] == "lp_text_c"]
check("a restart record does not cover MCL's screen", not any(t.startswith("LAST RESTART") for t in texts) and "TAP HERE TO CLEAR" not in texts
      and [c for c in calls if c[0] == "lp_box" and c[4] == 2], texts[:4])

sig_a = entry("mcl_page_sig")
check("page signature depends on the picture", sig_a == entry("mcl_page_sig"))
page_down(1, W - 10, 150)                                        # touch id 1 on SHIFT (B3: the bench's stop)
check("a SHIFT touch presses B3 (stop) and shows it pressed", screen()[3].endswith("B0010"), screen()[3])
sig_b = entry("mcl_page_sig")
check("the signature changes while a button is held", sig_a != sig_b)
entry("mcl_page_up", 1)
check("the release returns the signature", entry("mcl_page_sig") != sig_b)
page_down(0, W // 2, 80)                                         # on the screen: ignored
check("touching the screen does nothing", screen()[3].endswith("B0000"))
page_down(2, 10, 50)                                             # NO (B1) on touch id 2
check("NO: B1, follow", "FOLLOW" in screen()[0], screen()[0])
check("the button shows in the button bits", screen()[3].endswith("B1000"), screen()[3])
entry("mcl_page_up", 2)
entry("mcl_page_up", 0)
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

# --- boots after any reset start MCL; the restart loop guard: the fourth boot in a row that comes less than a minute after the one before skips it
boots, guis = [], []
e.stub(r.b["mcl_boot"], lambda: boots.append(1))
e.uc.mem_write(REC, struct.pack("<IHHI", 0x31435243, 6, 1, (1 << 24) | (1 << 22)) + bytes([1, 2, 0, 0, 0, 0]) + bytes(0x60))
GUARD = 0x38800690
e.w32(GUARD, 0x12345677)                                          # what an older build left there: no marker, counts as 0
for n in (1, 2, 3):
    e.w32(0x2405FFD0, 0)
    r.boot()
check("three boots in a row: MCL boots every time (a stale guard word without its marker counts as 0)", boots == [1] * 3 and e.r32(GUARD) == 0x4D434C03, (boots, hex(e.r32(GUARD))))
texts = [c[5] for c in draw() if c[0] == "lp_text_c"]
check("... no restart record, no 'MCL IS OFF', no tap prompt on the page", not any("RESTART" in t or "MCL IS OFF" in t or "TAP" in t for t in texts), texts[:6])
e.w32(0x2405FFD0, 0)
r.boot()
check("the fourth quick boot in a row: MCL is not started", boots == [1] * 3, boots)
texts = [c[5] for c in draw() if c[0] == "lp_text_c"]
check("... its page says MCL NOT RUNNING and where MCL crashed (the crash record), no tap prompt", "MCL NOT RUNNING" in texts and any(t.startswith("LAST RESTART") for t in texts) and any(t.startswith("PC ") for t in texts) and not any("TAP" in t for t in texts), texts[:6])
e.w32(GUARD, 0x4D434C04)
e.w32(0x2405FFD0, 0)
e.w32(GUARD, 0x4D434C00)                                          # (what a minute of running leaves: cpu.c's report clears the count; test_cpu.py)
r.boot()
check("after a minute of running, the next boot starts MCL again", boots == [1] * 4, boots)

# --- cpu_boot: MCL's SDRAM region and status words cleared at every boot (a warm reset leaves the last run's ready flags in the SDRAM)
e.uc.mem_write(0xC3F80000, b"\xa5" * 0x80000)
e.w32(0x38800920, 0x4D434C42)
FMC_MAPPED = True
try:
    e.uc.mem_map(0x52004000, 0x1000)
except Exception:
    pass
e.w32(0x52004000, 0x80000000)
e.w32(0x24000088, 0xC3F80000)
e.stub(0x08046340)
e.call("cpu_boot")
ram = bytes(e.uc.mem_read(0xC3F80000, 0x80000))
check("cpu_boot: the blob's whole SDRAM region is zero, its status words too", ram == bytes(0x80000) and e.r32(0x38800920) == 0, (ram.count(0), hex(e.r32(0x38800920))))
e.uc.mem_write(0xC3F80000, b"\xa5" * 16)
e.w32(0x52004000, 0)
e.call("cpu_boot")
check("... not while the FMC is off (the SDRAM not up yet)", bytes(e.uc.mem_read(0xC3F80000, 16)) == b"\xa5" * 16)

print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
