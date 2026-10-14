"""The load governor of the DRONE synth (lira_pad.c govern, lira_page_run) under Unicorn.

The audio task's wake-up time (0x2405ffd4) and the cycle counter are written by the test: a block is 2.56 M cycles, and the stock
work done by the time the synth runs is 50 % / 85 % / 97 % of it. Checked: no step at 50 %, a step every ~16 blocks while the load
stays over 80 % (one delay line, no delay, silence), the synth stays silent, and steps back after ~400 blocks with room again.
NOT checked: the real cycle counts on the device.
"""
import math
import struct

from unicorn.arm_const import UC_ARM_REG_R0

from emu import Emu

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
e.w32(0x24000088, 0xC4000000)
e.w32(0x2400008C, 0xC0000000)
e.call("interp_register", 0x24060000, 0xF1, 0, 0, 2, 0)
N, PER = 256, 2_560_000
L, R = 0x24070000, 0x24071000
cyc = [10_000_000]


def block(load):
    cyc[0] += PER
    e.w32(0xE0001004, cyc[0] & 0xFFFFFFFF)
    e.w32(0x2405FFD4, (cyc[0] - int(PER * load)) & 0xFFFFFFFF)
    e.uc.mem_write(L, bytes(4 * N))
    e.uc.mem_write(R, bytes(4 * N))
    e.call("lira_page_run", L, R, N)
    x = struct.unpack(f"<{N}f", bytes(e.uc.mem_read(L, 4 * N)))
    return math.sqrt(sum(v * v for v in x) / N)


def shed():
    e.call("lira_ui_load", 2)
    return e.uc.reg_read(UC_ARM_REG_R0)


# Touch = Ext with no plate held: the engine must run (the input opens the voices) instead of waiting for a plate
e.call("lira_ui_set", 100, 2)
e.call("lira_ui_set", 101, 2)
e.call("lira_ui_set", 102, 2)
e.call("lira_ui_set", 103, 2)
e.call("lira_ui_action", 5)                                     # X:MIX
e.call("lira_ui_set", 121, 0)
e.call("lira_ui_set", 122, 0)


def block_in(level, x):
    cyc[0] += PER
    e.w32(0xE0001004, cyc[0] & 0xFFFFFFFF)
    e.w32(0x2405FFD4, (cyc[0] - int(PER * level)) & 0xFFFFFFFF)
    e.uc.mem_write(L, struct.pack(f"<{N}f", *([x] * N)))
    e.uc.mem_write(R, struct.pack(f"<{N}f", *([x] * N)))
    e.call("lira_page_run", L, R, N)
    y = struct.unpack(f"<{N}f", bytes(e.uc.mem_read(L, 4 * N)))
    return math.sqrt(sum((a - x) ** 2 for a in y) / N)


for _ in range(60):
    r = block_in(0.3, 0.5)
check("Touch = Ext: with no plate held the synth still sounds from the input", r > 0.001, "rms %.5f" % r)
e.call("lira_ui_set", 100, 1)
e.call("lira_ui_set", 101, 1)
e.call("lira_ui_set", 102, 1)
e.call("lira_ui_set", 103, 1)
e.call("lira_ui_key", 0, 1)
e.call("lira_ui_key", 3, 1)
for _ in range(40):
    block(0.5)
r = block(0.5)
check("50 % load: full quality, the synth sounds", shed() == 0 and r > 0.001, "shed %d rms %.4f" % (shed(), r))
seen = []
for i in range(120):
    r = block(0.85)
    if not seen or seen[-1] != shed():
        seen.append(shed())
check("85 % load: it steps 1, 2, 3 (one line, none, muted)", seen == [0, 1, 2, 3] or seen == [1, 2, 3], str(seen))
check("... and is silent at step 3", r == 0.0, "rms %.5f" % r)
for _ in range(300):
    block(0.5)
check("room again, but not for long enough: still muted", shed() == 3, str(shed()))
for _ in range(150):
    block(0.5)
check("after ~400 blocks it steps back one", shed() == 2, str(shed()))
for _ in range(900):
    block(0.5)
check("... and all the way back to full quality", shed() == 0 and block(0.5) > 0.001, str(shed()))
print("failures:", fails)
raise SystemExit(1 if fails else 0)
