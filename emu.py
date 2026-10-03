"""Unicorn harness for running the patched image's functions against fake objects."""
import struct
import subprocess

from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_THUMB, Uc
from unicorn.arm_const import UC_CPU_ARM_MAX
from unicorn.arm_const import (UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0,
                               UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP)

BASE = 0x08040000
STACK, RET = 0x2407F000, 0x0807FFF0
ARGS = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)


class _Stubs(dict):
    """address -> handler; each address gets its own code hook (one hook on every instruction is ~50x slower)."""

    def __init__(self, uc):
        super().__init__()
        self.uc, self.hooked = uc, set()

    def __setitem__(self, addr, fn):
        if addr not in self.hooked:
            self.hooked.add(addr)
            self.uc.hook_add(UC_HOOK_CODE, lambda uc, a, size, _: self[a]() if a in self else None, begin=addr, end=addr)
        super().__setitem__(addr, fn)


class Emu:
    def __init__(self, image, elf):
        self.sym = {l.split()[2]: int(l.split()[0], 16) for l in subprocess.run(
            ["arm-none-eabi-nm", elf], capture_output=True, text=True, check=True).stdout.splitlines()}
        self.uc = uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        uc.ctl_set_cpu_model(UC_CPU_ARM_MAX)          # needs FPv5 (vfma, vsel) like the Cortex-M7
        uc.mem_map(0x08000000, 0x200000)
        uc.mem_write(BASE, open(image, "rb").read())
        uc.mem_map(0x24000000, 0x80000)
        uc.mem_write(RET, b"\x00\xbf\x00\xbf")
        uc.reg_write(UC_ARM_REG_C1_C0_2, 0xF << 20)   # FPU on
        uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        self.stubs, self.calls = _Stubs(uc), []

    def arg(self, n):
        return self.uc.reg_read(ARGS[n])

    def ret(self, value=None):
        if value is not None:
            self.uc.reg_write(UC_ARM_REG_R0, value)
        self.uc.reg_write(UC_ARM_REG_PC, self.uc.reg_read(UC_ARM_REG_LR))

    def stub(self, addr, fn=None, value=None):
        """Replace the firmware function at addr; fn() may record into self.calls."""
        def run():
            if fn:
                fn()
            self.ret(value)
        self.stubs[addr] = run

    def call(self, target, *args, count=2_000_000):
        addr = self.sym[target] if isinstance(target, str) else target
        for reg, v in zip(ARGS, args):
            self.uc.reg_write(reg, v & 0xFFFFFFFF)
        self.uc.reg_write(UC_ARM_REG_SP, STACK)
        self.uc.reg_write(UC_ARM_REG_LR, RET | 1)
        self.calls.clear()
        self.uc.emu_start(addr | 1, RET, count=count)
        assert self.uc.reg_read(UC_ARM_REG_PC) == RET, f"did not return from {target}"
        return list(self.calls)

    def r8(self, a):
        return self.uc.mem_read(a, 1)[0]

    def r16(self, a):
        return struct.unpack("<H", self.uc.mem_read(a, 2))[0]

    def r32(self, a):
        return struct.unpack("<i", self.uc.mem_read(a, 4))[0]

    def w32(self, a, v):
        self.uc.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))

    def cstr(self, a):
        return bytes(self.uc.mem_read(a, 32)).split(b"\0")[0].decode()
