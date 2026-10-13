"""The LIRA page's engine instance (lira_pad.c: lira_page_run, lira_ui_*) under Unicorn, on the patched image.

Real firmware code is not involved here beyond the cave; the stubs are the registrars and the SDRAM allocator.
Checked: silent until a sensor is on, sound with one, level control, the tail ends (no processing after ~14 s without a
sensor), per-pair Touch / FM Source / FM Depth stored and applied, momentary flag, drag state per finger, repeated
blocks add into the buffers (the looper's bus) instead of replacing them.
NOT checked: the drawing (looper_page.c / lira_ui.inc), the sound against the pad version, CPU on the device.
"""
import math
import struct

from unicorn.arm_const import UC_ARM_REG_R0

from emu import STACK, Emu

IMG = "out/solo+slice+duck+chord+cond+filter+cpu+od+comp+seqfix+fx2+munchi+lira+cave/BLACKBOX.bin"
e = Emu(IMG, "out/cave.elf")
e.uc.mem_map(0xC0000000, 0x400000)
e.uc.mem_map(0x38800000, 0x1000)
e.uc.mem_map(0x58024000, 0x1000)
e.uc.mem_map(0xE0000000, 0x10000)
fails = 0


def check(name, ok, info=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + ("  " + info if info else ""))
    fails += not ok


e.stub(0x0808C0D8)
e.stub(0x0808C12C)
e.stub(0x0804436C, value=0xC0000000)
e.call("interp_register", 0x24060000, 0xF1, 0, 0, 2, 0)

N = 256
L, R = 0x24070000, 0x24071000
e.uc.mem_map(0x24070000, 0x4000) if False else None


def run(blocks=1, pre=0.0):
    out = []
    for _ in range(blocks):
        e.uc.mem_write(L, struct.pack(f"<{N}f", *([pre] * N)))
        e.uc.mem_write(R, struct.pack(f"<{N}f", *([pre] * N)))
        e.call("lira_page_run", L, R, N)
        out += struct.unpack(f"<{N}f", bytes(e.uc.mem_read(L, 4 * N)))
    return out


def rv(name, *args):
    if len(args) > 4:
        e.w32(STACK, args[4])
        e.w32(STACK + 4, args[5] if len(args) > 5 else 0)
    e.call(name, *args[:4])
    v = e.uc.reg_read(UC_ARM_REG_R0)
    return v - (1 << 32) if v & 0x80000000 else v


def rms(x):
    return math.sqrt(sum(v * v for v in x) / max(1, len(x)))


try:
    e.uc.mem_read(L, 4)
except Exception:
    e.uc.mem_map(0x24070000, 0x4000)

check("no sensor, nothing: the buffers are untouched", rms(run(4)) == 0.0)
e.call("lira_ui_key", 0, 1)
e.call("lira_ui_key", 3, 1)
out = run(120)
a = rms(out[-40 * N:])
check("sensors 1 and 4 on: sound", a > 0.003, "rms %.5f" % a)
check("it adds into the bus (pre-existing 0.25 stays)", abs(sum(run(1, 0.25)[:N]) / N - 0.25) < 0.2)
e.call("lira_ui_set", 120, 350)
b = rms(run(40)[-20 * N:])
e.call("lira_ui_set", 120, 700)
check("LEVEL 350 is quieter than 700", b < a * 0.8, "%.5f vs %.5f" % (b, a))
check("ui get/set round trip", rv("lira_ui_get", 120) == 700 and rv("lira_ui_get", 104 + 2) == 0)
e.call("lira_ui_set", 105, 3)
e.call("lira_ui_set", 100, 2)
e.call("lira_ui_set", 109, 600)
check("per pair source / touch / FM depth stored", rv("lira_ui_get", 105) == 3 and rv("lira_ui_get", 100) == 2
      and rv("lira_ui_get", 109) == 600)
e.call("lira_ui_set", 105, 99)
check("source clamps to the list", rv("lira_ui_get", 105) == 7)
r = rms(run(20)[-5 * N:])
check("still finite with FM Source Pair4 / Touch Follow", r == r and r < 2.0, "%.4f" % r)
check("momentary toggles", rv("lira_ui_momentary", 0) == 0 and rv("lira_ui_momentary", 1) == 1 and
      rv("lira_ui_momentary", 1) == 0)
rv("lira_ui_drag", 2, 0, 3, 7, 123, 456)
check("drag state per finger", rv("lira_ui_drag", 2, 1, 0, 0, 0, 0) == 3 and rv("lira_ui_drag", 2, 2, 0, 0, 0, 0) == 7
      and rv("lira_ui_drag", 6, 3, 0, 0, 0, 0) == 123 and rv("lira_ui_drag", 2, 4, 0, 0, 0, 0) == 456)
e.call("lira_ui_key", 0, 0)
e.call("lira_ui_key", 3, 0)
e.call("lira_ui_set", 121, 900)
e.call("lira_ui_set", 126, 300)
e.call("lira_ui_set", 131, 800)
check("the split controls (Hold A, Dly Mod 2, Sharp pair 2) are stored apart", rv("lira_ui_get", 121) == 900 and rv("lira_ui_get", 122) == 0 and rv("lira_ui_get", 126) == 300 and rv("lira_ui_get", 131) == 800)
e.call("lira_ui_set", 121, 0)
check("sensor flags read back", rv("lira_ui_key_get", 0) == 0)
tail = rms(run(60)[-N:])
check("after release the tail still rings (release is seconds)", tail > 0.0005, "%.5f" % tail)
silent = rms(run(14 * 48000 // N + 40)[-N:])
check("after ~14 s the instance stops (buffers untouched)", silent == 0.0, "%.6f" % silent)
e.call("lira_ui_set", 121, 900)
droning = rms(run(120)[-40 * N:])
check("Hold raised: the voices drone with no plate pressed", droning > 0.003, "%.5f" % droning)
e.call("lira_ui_set", 121, 0)
print("failures:", fails)
raise SystemExit(1 if fails else 0)
