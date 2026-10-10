"""The TRIG tab (src/t2m_page.c inside src/looper_page.c) under Unicorn, on the patched image.

Uses test_looper.py's setup (the fake pool, the mixer view with the real 256 x 224 grid, the drawing stubs) by running
that file up to its first page draw, then: the TRIG tab from the footer, the page's switches, rows and knobs, the
redraw signature, and a PNG of the page (out/t2m_test/trig_page.png, colours from the firmware's palette).

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


# struct offsets from the header (same layout on the host)
os.makedirs("out/t2m_test", exist_ok=True)
open("out/t2m_test/off2.c", "w").write(
    '#include <stdio.h>\n#include <stddef.h>\n#include "../../src/t2m_bb.h"\nint main(void){printf("%zu %zu %zu %zu %zu '
    '%zu %zu %zu %zu %zu %zu %zu %zu %zu\\n", offsetof(struct t2m_mem, on), offsetof(struct t2m_mem, port), '
    'offsetof(struct t2m_mem, ui), offsetof(struct t2m_mem, p), sizeof(t2m_params), offsetof(t2m_params, note), '
    'offsetof(t2m_params, note_on), offsetof(t2m_params, vel_mode), offsetof(t2m_params, vel_fixed), '
    'offsetof(t2m_params, cc_on), offsetof(t2m_params, cc_num), offsetof(t2m_params, sens), '
    'offsetof(struct t2m_mem, hits), offsetof(struct t2m_mem, peak));}\n')
subprocess.run(["gcc", "-o", "out/t2m_test/off2", "out/t2m_test/off2.c"], check=True)
(O_ON, O_PORT, O_UI, O_P, PSIZE, OP_NOTE, OP_NOTE_ON, OP_VMODE, OP_VFIX, OP_CC_ON, OP_CC_NUM, OP_SENS, O_HITS,
 O_PEAK) = map(int, subprocess.run(["out/t2m_test/off2"], capture_output=True, text=True, check=True).stdout.split())

# memory for Trigger2MIDI: the allocator stubbed (the looper test's boot ran with the real one: no memory)
MEM = 0x30030000
e.stub(0x080443F0, fn=lambda: e.uc.reg_write(A.UC_ARM_REG_R0, 3 << 20))
e.stub(0x08044404, fn=lambda: e.uc.reg_write(A.UC_ARM_REG_R0, MEM))
e.call("t2m_boot")
check("Trigger2MIDI has its memory", e.r32(0x38800984) == MEM)


def p8(k, off):
    return e.r8(MEM + O_P + k * PSIZE + off)


def touch(kind, x_rel, d):
    """As test_looper.py: fill-space point x = 3 + x_rel, y = 224 - d."""
    e.uc.mem_write(PTA, struct.pack("<2i", 3 + x_rel, 224 - d))
    e.call({"down": "solo_touch_down", "move": "solo_touch_move", "up": "solo_touch_up"}[kind], VIEW, PTA, 0)


MSG = 0x2403A000


def knob(i, counts):
    """As test_looper.py: knob i (track i's), counts; the left encoders arrive swapped."""
    i = i ^ 1 if i < 2 else i
    e.uc.mem_write(MSG, struct.pack("<H", 0x32) + bytes(10) + struct.pack("<h", i) + bytes(2) + struct.pack("<h", counts))
    e.call("looper_view_msg", VIEW, MSG)


# ---- the TRIG tab: sixth button of the footer (x 3 + 5 x 36 = 183 .. 217, footer below d = 209)
f = draw(0)
check("the footer has a sixth tab", any(x[0] == 3 + 3 + 5 * 36 and x[2] == 34 and x[3] == 1 for x in f), [x for x in f if x[3] == 1][:4])
touch("down", 190, 216)
touch("up", 190, 216)
check("tapping it opens the TRIG tab", e.r8(PAGE + 76) == 5, e.r8(PAGE + 76))
e.call("looper_page_tab")
check("looper_page_tab reports it as tab 6", e.uc.reg_read(A.UC_ARM_REG_R0) == 6)
f = draw(0)
check("TRIG page: everything inside the screen (x 3..317, y 0..224)",
      all(x[0] >= 3 and x[0] + x[2] <= 317 and x[1] >= 0 and x[1] + x[3] <= 224 for x in f) and
      all(3 <= px < 317 and 0 <= py < 224 for px, py, _ in pixels), [x for x in f if not (x[0] >= 3 and x[0] + x[2] <= 317)][:3])
check("TRIG page: no looper track columns (no 71 px record boxes)", not any(x[2] == 71 for x in f))
check("TRIG page: text drawn", len(pixels) > 2000, len(pixels))


# ---- switches (row at d 18 .. 34): IN L 4..42, IN R 46..84, ON 104..144, TRS 162..200, USB 204..242
def tap(x, d):
    touch("down", x, d)
    touch("up", x, d)


tap(60, 26)
check("IN R: the page shows input R", e.r8(MEM + O_UI) == 1)
tap(120, 26)
check("ON: switches input R on", e.r8(MEM + O_ON + 1) == 1)
tap(180, 26)
check("TRS: input R's TRS output off (USB stays)", e.r8(MEM + O_PORT + 1) == 2, e.r8(MEM + O_PORT + 1))
tap(180, 26)
check("TRS again: back on", e.r8(MEM + O_PORT + 1) == 3)
tap(20, 26)
check("IN L: back to input L, R keeps its settings", e.r8(MEM + O_UI) == 0 and e.r8(MEM + O_ON + 1) == 1)

# ---- rows (d from 54, 34 px each): tap the NOTE row, then the knobs (knob n = value n of the row)
tap(150, 54 + 2 * 34 + 10)
check("tapping the NOTE row hands it to the knobs", e.r8(MEM + O_UI + 1) == 2, e.r8(MEM + O_UI + 1))
knob(0, 40)
check("knob 1, one step (40 counts) up: NOTE 38 -> 39", p8(0, OP_NOTE) == 39, p8(0, OP_NOTE))
knob(0, 20)
knob(0, 20)
check("two half steps make a step: 40", p8(0, OP_NOTE) == 40, p8(0, OP_NOTE))
knob(0, -40 * 60)
check("down past 0: NOTE OFF (no note)", p8(0, OP_NOTE_ON) == 0, (p8(0, OP_NOTE_ON), p8(0, OP_NOTE)))
knob(0, 40 * 39)
check("back up: note on again at 38", p8(0, OP_NOTE_ON) == 1 and p8(0, OP_NOTE) == 38, (p8(0, OP_NOTE_ON), p8(0, OP_NOTE)))
knob(1, 40 * 100)
check("knob 2: VEL from DYN to fixed 100", p8(0, OP_VMODE) == 1 and p8(0, OP_VFIX) == 100, (p8(0, OP_VMODE), p8(0, OP_VFIX)))
knob(1, -40 * 200)
check("... and back down to DYN", p8(0, OP_VMODE) == 0)
tap(150, 54 + 3 * 34 + 10)
knob(0, 40 * 75)
check("CC row, knob 1: CC OFF -> CC 74", p8(0, OP_CC_ON) == 1 and p8(0, OP_CC_NUM) == 74, (p8(0, OP_CC_ON), p8(0, OP_CC_NUM)))
tap(150, 54 + 10)
knob(0, 40 * 20)
sens = struct.unpack("<f", e.uc.mem_read(MEM + O_P + OP_SENS, 4))[0]
check(f"DETECT row, knob 1: SENS 2.00 -> {sens:.2f} (0.1 steps above 2)", abs(sens - 4.0) < 1e-4, sens)
knob(0, -40 * 500)
sens = struct.unpack("<f", e.uc.mem_read(MEM + O_P + OP_SENS, 4))[0]
check("SENS stops at 0.1", abs(sens - 0.1) < 1e-6, sens)
knob(0, 40 * 38)
check("the input R settings did not move", p8(1, OP_NOTE) == 38 and p8(1, OP_CC_ON) == 0)
check("knobs on the TRIG page never move the looper's faders", abs(tr(0, O_LEVEL, "f") - 1.0) < 1e-6, tr(0, O_LEVEL, "f"))

# ---- redraw signature follows hits and the meter
e.call("t2m_page_sig")
s0 = e.uc.reg_read(A.UC_ARM_REG_R0)
e.w32(MEM + O_HITS, 5)
e.call("t2m_page_sig")
check("the page redraws when a hit is counted", e.uc.reg_read(A.UC_ARM_REG_R0) != s0)

# ---- a picture: level and a few hits, so the meter and top bar show something
e.uc.mem_write(MEM + O_PEAK, struct.pack("<f", 0.3))
e.w32(MEM + O_HITS, 128)
f = draw(0)
fw = open("firmware/BLACKBOX-3.1.9.bin", "rb").read()
pal = [struct.unpack_from("<I", fw, 0x080F1D80 - 0x08040000 + 4 * i)[0] for i in range(48)]
W, H = 320, 224
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
raw = b"".join(b"\x00" + bytes(img[r * W * 3:(r + 1) * W * 3]) for r in range(H))
S = 3                                                     # scaled up for a phone screen
big = b"".join(b"\x00" + b"".join(bytes(img[(r // S * W + c // S) * 3:(r // S * W + c // S) * 3 + 3]) for c in range(W * S))
               for r in range(H * S))


def png(path, w, h, data):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(data, 9)) + chunk(b"IEND", b""))


png("out/t2m_test/trig_page.png", W * S, H * S, big)
print("wrote out/t2m_test/trig_page.png")

print("ALL PASS" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
