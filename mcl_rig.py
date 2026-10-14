"""Shared Unicorn rig for the MCL blob tests (test_mcl_midi.py, test_mcl_clock.py): the image with the blob, a modelled USART6 and wire."""
import glob
import os
import shutil
import struct
import subprocess

from unicorn import UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0

from emu import Emu

NAMES = "solo slice duck chord cond filter cpu od comp seqfix fx2 munchi looper clkmon cave mclblob".split()
BASE, RAM, RAM_END = 0x08040000, 0xC3F80000, 0xC4000000
U = 0x40011400
MGR = 0x24002E88


def build(test, m4=False, m5=False):
    env = dict(os.environ, M3TEST="1" if test else "", M4TEST="1" if m4 else "", M5TEST="1" if m5 else "")
    subprocess.run(["tools/mcl/build_blob.sh"], check=True, env=env, capture_output=True)
    subprocess.run(["python3", "patch.py"] + NAMES, check=True, capture_output=True)
    img = glob.glob("out/*+cave+mclblob/BLACKBOX.bin")[0]
    d = "out/m3test" if test else ("out/m4test" if m4 else ("out/m5test" if m5 else "out/m3plain"))
    os.makedirs(d, exist_ok=True)
    shutil.copy(img, d + "/BLACKBOX.bin")
    shutil.copy("out/mcl.elf", d + "/mcl.elf")
    return d


def syms(path):
    return {l.split()[2]: int(l.split()[0], 16) for l in subprocess.run(["arm-none-eabi-nm", path], capture_output=True, text=True, check=True).stdout.splitlines() if len(l.split()) == 3}


def emulate_primask(rg):
    """Unicorn's ARM model has no M-profile special registers (mrs / msr / cpsid / cpsie are invalid instructions there). The test copy of the
    blob in emulator memory gets them replaced by `movs rN,#0` / nop: single-threaded here, so there is nothing to mask. The image on disk is untouched.
    Instruction addresses come from the blob's own disassembly (objdump), which knows where the literal pools are."""
    dump = subprocess.run(["arm-none-eabi-objdump", "-d", "--no-show-raw-insn", rg.elf], capture_output=True, text=True, check=True).stdout
    for line in dump.splitlines():
        f = line.split("\t")
        if len(f) < 2 or not f[0].strip().endswith(":"):
            continue
        addr, mn = int(f[0].strip()[:-1], 16), f[1].strip()
        if mn == "mrs":
            rn = int(f[2].split(",")[0].strip()[1:])
            rep = struct.pack("<HH", 0x2000 | (rn << 8), 0xBF00)              # movs rN, #0 ; nop
        elif mn == "msr":
            rep = b"\x00\xbf\x00\xbf"
        elif mn in ("cpsid", "cpsie"):
            rep = b"\x00\xbf"
        else:
            continue
        rg.e.uc.mem_write(addr, rep)


class Rig:
    """The unit: image + modelled USART6 + wire."""

    def __init__(self, d):
        self.img, self.elf, self.b = d + "/BLACKBOX.bin", d + "/mcl.elf", syms(d + "/mcl.elf")
        e = self.e = Emu(self.img, "out/cave.elf")
        u = e.uc
        u.mem_map(0xE0000000, 0x100000)
        u.mem_map(0x38800000, 0x1000)
        u.mem_map(0x58024000, 0x1000)
        u.mem_map(0x40011000, 0x1000)
        u.mem_map(RAM, RAM_END - RAM)
        u.mem_write(RAM, b"\xa5" * (RAM_END - RAM))
        u.mem_write(0x38800000, b"\x5a" * 0x1000)
        e.w32(0x24000088, 0xC3F00000)
        e.w32(0x2400008C, 0xC0000000 + 40 * 1024 * 1024)
        e.w32(0x24009914 + 0x80, 0x20)                         # HAL gState READY
        e.w32(MGR + 0x20D - 3, 0x01000000)                     # stock ring 0 active flag byte at mgr+0x20d
        e.stub(0x080A3DEC)
        e.uc.mem_write(0x24060000, bytes(0x60))
        e.uc.mem_write(0x24060000 + 0x31, e.cstr(0x080CF290).encode() + b"\0")
        e.uc.mem_write(0x2405FFD0, b"\xa5" * 36)
        self.sent, self.stock_calls, self.irq_calls = [], [], 0
        e.stub(0x08044E04, lambda: self.stock_calls.append((e.arg(0), e.arg(1), e.arg(2))), value=1)
        e.stub(0x080462E4, self._stock_irq)
        u.hook_add(UC_HOOK_MEM_WRITE, lambda uc, t, a, sz, v, _: self.sent.append(v & 0xFF), begin=U + 0x28, end=U + 0x2B)
        self.dev = None
        self.clock = 0
        emulate_primask(self)

    def _stock_irq(self):
        self.irq_calls += 1

    def call(self, name, *a):
        self.e.call(self.b[name], *a)
        return self.e.uc.reg_read(UC_ARM_REG_R0)

    def boot(self):
        self.e.call("cpu_text_b", 0x24060000, 0x080CF290)
        return self.e.cstr(0x24060000 + 0x31)

    def isr(self, isr_flags=0x80 | 0x40):
        self.e.w32(U + 0x1C, isr_flags)
        self.e.call(self.b["mcl_usart6_isr"])

    def rx(self, byte, port=0):
        self.e.call(0x080446C4, MGR, byte, port)

    def pump(self):
        """Hardware transmit side: while CR1.TXEIE is set the TXE interrupt fires; each byte goes out on the wire (and into the device model)."""
        n = 0
        while self.e.r32(U) & 0x80 and n < 100000:
            before = len(self.sent)
            self.isr()
            for b in self.sent[before:]:
                if self.dev:
                    self.dev(b)
            n += 1
        return n

    def tx(self, data):
        buf = 0x24070000
        self.e.uc.mem_write(buf, bytes(data))
        self.e.call(self.b["mcl_tx_hook"], 0, buf, len(data))
        return self.e.uc.reg_read(UC_ARM_REG_R0)




# ---- the clock rig (test_mcl_clock.py, test_mcl_sync.py): TIM6 and the cycle counter modelled
from unicorn import UC_HOOK_MEM_READ

TIM6, DWT, SYST = 0x40001000, 0xE0001004, 0xE000E010


def clock_rig(core_hz=399_000_000, tim_hz=200_000_000, m4=False, m5=False):
    d = build(False, m4, m5)
    r = Rig(d)
    e = r.e
    e.uc.mem_map(0x40001000 - 0x1000 + 0x1000, 0x1000) if False else e.uc.mem_map(0x40001000, 0x1000)
    e.w32(0xE000E014, core_hz // 1000 - 1)                       # SysTick reload: the firmware's 1 kHz tick
    e.w32(SYST, 7)
    e.w32(0x580244E8, 0)
    st = {"dwt": 0, "step": 900}

    def dwt_read(uc, t, a, sz, v, _):
        st["dwt"] += st["step"]                                   # every read of the cycle counter: 900 cycles (0 once a test sets the time itself)
        uc.mem_write(DWT, struct.pack("<I", st["dwt"] & 0xFFFFFFFF))

    def cnt_read(uc, t, a, sz, v, _):                              # TIM6 counter follows the cycle counter at tim_hz / core_hz (PSC = 0 while calibrating)
        psc = struct.unpack("<I", uc.mem_read(TIM6 + 0x28, 4))[0]
        uc.mem_write(TIM6 + 0x24, struct.pack("<I", int(st["dwt"] * tim_hz / core_hz / (psc + 1)) & 0xFFFF))

    e.uc.hook_add(UC_HOOK_MEM_READ, dwt_read, begin=DWT, end=DWT + 3)
    e.uc.hook_add(UC_HOOK_MEM_READ, cnt_read, begin=TIM6 + 0x24, end=TIM6 + 0x27)
    r.call("mcl_boot")
    r.st = st
    return r


