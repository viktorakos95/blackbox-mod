"""prof_lira.py [scenario]: the DRONE-8 engine (src/lira.c) alone on the Cortex-M7 build, run under Unicorn, instruction
counts per source line and per function for one 256-frame block, plus a rough cycle estimate (one cycle an instruction,
14 for a VDIV/VSQRT: the M7's divider is not pipelined). Dual issue and cache misses are not modelled: compare builds
with it, do not read it as the device's load."""
import collections
import os
import struct
import subprocess
import sys
import tempfile

from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_THUMB, Uc
from unicorn.arm_const import (UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0,
                               UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_CPU_ARM_MAX)

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp()
ELF = os.path.join(TMP, "lira.elf")
LD = os.path.join(TMP, "l.ld")
open(LD, "w").write("SECTIONS { . = 0x08040000; .text : { *(.text*) *(.rodata*) } . = 0x24000000; .data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } }\n")
subprocess.check_call(["arm-none-eabi-gcc", "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16", "-Os", "-g",
                       "-ffreestanding", "-nostdlib", "-fno-builtin", "-T", LD, "-Wl,-e,0", "-o", ELF,
                       os.path.join(HERE, "src", "lira.c")])
BIN = ELF + ".bin"
subprocess.check_call(["arm-none-eabi-objcopy", "-O", "binary", "-j", ".text", ELF, BIN])
code = open(BIN, "rb").read()
sym = {}
for l in subprocess.run(["arm-none-eabi-nm", "-S", ELF], capture_output=True, text=True).stdout.splitlines():
    f = l.split()
    if len(f) == 4 and f[2] in "tT":
        sym[f[3]] = (int(f[0], 16), int(f[1], 16))
funcs = sorted((a, a + s, n) for n, (a, s) in sym.items())

BASE, STACK, RET = 0x08040000, 0x2407F000, 0x0807FFF0
uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
uc.ctl_set_cpu_model(UC_CPU_ARM_MAX)
uc.mem_map(0x08000000, 0x200000)
uc.mem_write(BASE, code)
uc.mem_write(RET, b"\x00\xbf\x00\xbf")
uc.mem_map(0x24000000, 0x80000)
uc.mem_map(0xC0000000, 0x100000)
uc.mem_map(0x20000000, 0x20000)
uc.reg_write(UC_ARM_REG_C1_C0_2, 0xF << 20)
uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
ARGS = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)


def call(name, *args, fargs=()):
    """hard-float AAPCS: ints in r0-r3 then the stack, floats in s0.."""
    from unicorn.arm_const import UC_ARM_REG_S0, UC_ARM_REG_S1, UC_ARM_REG_S2, UC_ARM_REG_S3, UC_ARM_REG_S4
    for r, v in zip(ARGS, args):
        uc.reg_write(r, v & 0xFFFFFFFF)
    for r, v in zip((UC_ARM_REG_S0, UC_ARM_REG_S1, UC_ARM_REG_S2, UC_ARM_REG_S3, UC_ARM_REG_S4), fargs):
        uc.reg_write(r, struct.unpack("<I", struct.pack("<f", v))[0])
    sp = STACK - 4 * len(args[4:])
    for k, v in enumerate(args[4:]):
        uc.mem_write(sp + 4 * k, struct.pack("<I", v & 0xFFFFFFFF))
    uc.reg_write(UC_ARM_REG_SP, sp)
    uc.reg_write(UC_ARM_REG_LR, RET | 1)
    uc.emu_start(sym[name][0] | 1, RET, count=200_000_000)


cnt = collections.Counter()
ON = [False]


def hk(uc_, a, size, _):
    if ON[0]:
        cnt[a] += 1


uc.hook_add(UC_HOOK_CODE, hk, begin=BASE, end=BASE + len(code))   # before anything runs: Unicorn does not hook blocks it already translated
L, D1, D2, OUT = 0xC0000000, 0xC0010000, 0xC0030000, 0x24070000
DLEN, SR, N = 44032, 48000.0, 256
NAMES = ("TUNE1 TUNE2 TUNE3 TUNE4 TUNE5 TUNE6 TUNE7 TUNE8 FAST12 FAST34 FAST56 FAST78 SHARP12 SHARP34 SHARP56 SHARP78 "
         "MOD12 MOD34 MOD56 MOD78 SOURCE12 SOURCE34 SOURCE56 SOURCE78 PITCH1234 PITCH5678 HOLD1234 HOLD5678 SWITCH "
         "TOTAL_FB VIBRATO DRV DST_MIX VOL F_A F_B ANDOR LINK TIME1 TIME2 FEEDBACK DEL_MIX MOD1 MOD2 DEL_MOD LFO_WAV "
         "QUANTIZE EXT WAVE").split()
P = {n: i for i, n in enumerate(NAMES)}

call("lira_init", L, D1, D2, DLEN, 0x6c697261, fargs=(SR,))
scen = sys.argv[1] if len(sys.argv) > 1 else "full"
setp = {"FEEDBACK": 70, "DEL_MIX": 80, "MOD12": 60, "MOD34": 60, "MOD56": 60, "MOD78": 60, "SHARP12": 40, "SHARP56": 40,
        "DST_MIX": 60, "MOD1": 40, "MOD2": 40}
if scen.endswith("0"):                  # e.g. full0: SHARP at zero everywhere
    scen = scen[:-1]
    setp.update(SHARP12=0, SHARP34=0, SHARP56=0, SHARP78=0)
if scen == "sine":
    setp["WAVE"] = 1
if scen == "slowdelay":
    setp.update(TIME1=127, TIME2=120)
for k, v in setp.items():
    call("lira_param", L, P[k], fargs=(float(v),))
TONE = os.environ.get("TONE")             # "width,drift,detune,body,harm": the page's tone, and the right channel on
if TONE:
    call("lira_tone", L, fargs=tuple(float(x) for x in TONE.split(",")))
    import subprocess as _sp
    src = '#include "src/lira.h"\nchar o_x[__builtin_offsetof(lira_t,out_r)];\n'
    asm = _sp.run(["arm-none-eabi-gcc", "-I.", "-mthumb", "-S", "-x", "c", "-", "-o", "-"], input=src, capture_output=True, text=True, check=True, cwd=HERE).stdout.splitlines()
    OFF_R = int([l.split()[1] for l in asm if l.strip().startswith(".space")][0])
    uc.mem_write(L + OFF_R, struct.pack("<I", 0x24072000))
keys = range(8) if scen != "idle" else ()
for k in keys:
    call("lira_key", L, k, 1)
for _ in range(40):                  # envelopes open
    call("lira_process", L, OUT, N)

heavy = set()                            # (objdump, not capstone: capstone stops at the first literal pool)
for l in subprocess.run(["arm-none-eabi-objdump", "-d", ELF], capture_output=True, text=True).stdout.splitlines():
    f = l.split("\t")
    if len(f) >= 3 and f[2].startswith(("vdiv", "vsqrt")):
        heavy.add(int(f[0].strip().rstrip(":"), 16))
NB = 4
ON[0] = True
for _ in range(NB):
    call("lira_process", L, OUT, N)
ON[0] = False
tot = sum(cnt.values())
cyc = tot + 13 * sum(v for a, v in cnt.items() if a in heavy)
print(f"scenario {scen}: {tot / NB / N:.0f} instructions a frame, ~{cyc / NB / N:.0f} cycles a frame (divides at 14), "
      f"{cyc / NB / 1000:.0f} k a 256 block = {100 * cyc / NB / 2560000:.1f} % of a 480 MHz block")
byf = collections.Counter()
for a, v in cnt.items():
    for s, e, n in funcs:
        if s <= a < e:
            byf[n] += v + (13 * v if a in heavy else 0)
            break
for n, v in byf.most_common(8):
    print(f"  {n:20s} {v / NB / N:7.0f} cyc/frame")
# per source line
addrs = sorted(cnt)
res = subprocess.run(["arm-none-eabi-addr2line", "-e", ELF] + [hex(a) for a in addrs], capture_output=True, text=True).stdout.splitlines()
byl = collections.Counter()
for a, loc in zip(addrs, res):
    byl[loc.split("/")[-1].split(" ")[0]] += cnt[a] + (13 * cnt[a] if a in heavy else 0)
for loc, v in byl.most_common(int(os.environ.get("TOP", "25"))):
    print(f"  {loc:18s} {v / NB / N:7.1f}")
