"""Rig for running MCL itself (the real core, M8) under Unicorn: the unit's image with the blob that links MCL (tools/mcl/build_mcl.sh), the stock FatFs on a FAT image
behind the SD layer (sdharness.py), a GUI context and a streamer context that alternate when the GUI side blocks, a modelled timer / cycle counter, the MIDI jack as a plain list.

MCL's task is run as the GUI context: `run(function, *args)` runs a blob function; whenever it sleeps or waits for the card (the blocking calls of the stock kernel that the blob makes) the
streamer context runs one pass (mcl_sd_pump) and, for sleeps, the modelled time moves on. Interrupt handlers (base timer, soft interrupts) are called by `tick()` like the hardware would.
Needs MCL_DIR and MCL_GFX (checkouts) to build; tests skip themselves when the MCL build is not available."""
import os
import shutil
import struct
import subprocess
import sys

from unicorn import UC_HOOK_MEM_READ
from unicorn.arm_const import (UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
                               UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_CPSR)

import sdharness as H
from emu import RET, STACK
from mcl_rig import MGR, Rig, emulate_primask, syms

NAMES = "solo slice duck chord cond filter cpu od comp seqfix fx2 munchi looper clkmon cave mclblob mclsd".split()
Q_CREATE, Q_SEND, Q_RECEIVE = 0x080883BE, 0x08088402, 0x08088756
CORE = 399_000_000
TASK_STACK = 0x2404F000
REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
        UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR]


def available():
    return bool(os.environ.get("MCL_DIR") and os.environ.get("MCL_GFX"))


def build_image(extra_env=None, out="out/mclcore-test"):
    env = dict(os.environ, M3TEST="", M4TEST="", M5TEST="", SDSPY="", SDTEST="", COMPACT="")
    env.update(extra_env or {})
    subprocess.run(["sh", "tools/mcl/build_mcl.sh"], check=True, capture_output=True, env=env)
    subprocess.run(["python3", "patch.py"] + NAMES, check=True, capture_output=True)
    os.makedirs(out, exist_ok=True)
    shutil.copy("out/" + "+".join(NAMES) + "/BLACKBOX.bin", out + "/BLACKBOX.bin")
    shutil.copy("out/mcl.elf", out + "/mcl.elf")
    return out


class Core:
    def __init__(self, d="out/mclcore-test", image="out/sd/core.img", files=()):
        os.makedirs("out/sd", exist_ok=True)
        H.make_image(image, files=files)
        self.rg = rg = Rig(d)
        self.e = e = rg.e
        self.uc = uc = e.uc
        self.b = rg.b
        uc.mem_map(0x40001000, 0x1000) if False else None
        # the timer / cycle counter model of clock_rig
        uc.mem_map(0x40001000, 0x1000)
        e.w32(0xE000E014, CORE // 1000 - 1)
        e.w32(0xE000E010, 7)
        e.w32(0x580244E8, 0)
        self.st = st = {"dwt": 0, "step": 900}
        DWT, TIM6 = 0xE0001004, 0x40001000

        def dwt_read(u, a, addr, size, v, _):
            st["dwt"] += st["step"]
            u.mem_write(DWT, struct.pack("<I", st["dwt"] & 0xFFFFFFFF))

        def cnt_read(u, a, addr, size, v, _):
            psc = struct.unpack("<I", bytes(u.mem_read(TIM6 + 0x28, 4)))[0]
            u.mem_write(TIM6 + 0x24, struct.pack("<I", int(st["dwt"] * 200_000_000 / CORE / (psc + 1)) & 0xFFFF))

        uc.hook_add(UC_HOOK_MEM_READ, dwt_read, begin=DWT, end=DWT + 3)
        uc.hook_add(UC_HOOK_MEM_READ, cnt_read, begin=TIM6 + 0x24, end=TIM6 + 0x27)
        try:                                                    # the stock kernel's variables the diagnostics read (pxCurrentTCB = a TCB-sized block)
            uc.mem_map(0x2401F000, 0x1000)
        except Exception:
            pass
        e.w32(0x2401F2A8, 0x2401F400)
        emulate_primask(rg)
        rg.call("mcl_boot")
        self.card = H.Card(image, emu=e)
        self.blocked = [False]
        self.sleeps = []
        e.stub(Q_CREATE, value=0x24050000)
        e.stub(Q_SEND, value=1)
        e.stub(Q_RECEIVE, self._q_receive)
        self.pump = 0

    def _q_receive(self):
        e = self.e
        ticks = e.arg(2)
        lr = self.uc.reg_read(UC_ARM_REG_LR) & ~1
        sleeping = self.b["bb_sleep_ms"] <= lr < self.b["bb_sleep_ms"] + 0x200      # the platform's sleep (also 50-tick slices), not the SD layer's wait
        if ticks == 50 and not sleeping:                         # the SD layer waiting for the streamer
            self.blocked[0] = True
            e.ret(1)
            self.uc.emu_stop()
        else:
            if ticks:
                self.sleeps.append(ticks)
                self.st["dwt"] += ticks * (CORE // 1000)         # time passes while a task sleeps
            e.ret(0)

    def service(self):
        uc = self.uc
        saved = [uc.reg_read(r) for r in REGS]
        for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), (0x24060000, 0, 0, 0)):
            uc.reg_write(r, v)
        uc.reg_write(UC_ARM_REG_SP, STACK - 0x10000)
        uc.reg_write(UC_ARM_REG_LR, RET | 1)
        self.pump += 1
        uc.emu_start(self.b["mcl_sd_pump"] | 1, RET, count=50_000_000)
        for r, v in zip(REGS, saved):
            uc.reg_write(r, v)

    def run(self, name, *args, count=200_000_000, max_loops=200_000):
        uc = self.uc
        addr = self.b[name] if isinstance(name, str) else name
        for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            uc.reg_write(r, v & 0xFFFFFFFF)
        uc.reg_write(UC_ARM_REG_SP, STACK)
        uc.reg_write(UC_ARM_REG_LR, RET | 1)
        pc = addr | 1
        for _ in range(max_loops):
            self.blocked[0] = False
            uc.emu_start(pc, RET, count=count)
            if not self.blocked[0]:
                return uc.reg_read(UC_ARM_REG_R0)
            self.service()
            pc = uc.reg_read(UC_ARM_REG_PC) | 1
        raise AssertionError("did not finish")

    def start(self, name):
        """begin a blob function that does not return (MCL's setup waits in its menu); `step` runs it on"""
        uc = self.uc
        uc.reg_write(UC_ARM_REG_SP, TASK_STACK)                  # its own stack: the helpers (call / irq / side) use others
        uc.reg_write(UC_ARM_REG_LR, RET | 1)
        self.pc = self.b[name] | 1

    def step(self, count=2_000_000):
        """let the started function run for `count` instructions (the card is served when it waits for it); the state stays, `step` again goes on"""
        uc = self.uc
        sp, lr = None, None
        for _ in range(10000):
            self.blocked[0] = False
            uc.emu_start(self.pc, RET, count=count)
            self.pc = uc.reg_read(UC_ARM_REG_PC) | 1
            if not self.blocked[0]:
                return
            self.service()
        raise AssertionError("step did not finish")

    def tick(self, ms=1):
        """the base timer's interrupts for `ms` milliseconds: the panel poll (soft 3) every 5 ms, the MIDI side (soft 2) every ms"""
        for i in range(ms):
            self.st["dwt"] += CORE // 1000
            if i % 5 == 0:
                self.irq("bb_swirq3_isr")
            self.irq("bb_swirq2_isr")

    def call(self, name, *args, count=20_000_000):
        """a short blob function called on the side (own stack, the interrupted context's registers kept); returns r0"""
        uc = self.uc
        saved = [uc.reg_read(r) for r in REGS]
        for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            uc.reg_write(r, v & 0xFFFFFFFF)
        uc.reg_write(UC_ARM_REG_SP, STACK - 0x20000)
        uc.reg_write(UC_ARM_REG_LR, RET | 1)
        uc.emu_start(self.b[name] | 1, RET, count=count)
        r0 = uc.reg_read(UC_ARM_REG_R0)
        for r, v in zip(REGS, saved):
            uc.reg_write(r, v)
        return r0

    def side(self, fn, *args):
        """run a Python-side helper that calls into the emulator (the Rig's USART pump, receive path) without disturbing the paused task's registers"""
        uc = self.uc
        saved = [uc.reg_read(r) for r in REGS]
        r = fn(*args)
        for r_, v in zip(REGS, saved):
            uc.reg_write(r_, v)
        return r

    def irq(self, name, count=20_000_000):
        """an interrupt handler of the blob, entered like the hardware would (own stack)"""
        uc = self.uc
        saved = [uc.reg_read(r) for r in REGS]
        uc.reg_write(UC_ARM_REG_SP, STACK - 0x20000)
        uc.reg_write(UC_ARM_REG_LR, RET | 1)
        uc.emu_start(self.b[name] | 1, RET, count=count)
        for r, v in zip(REGS, saved):
            uc.reg_write(r, v)

    def screen(self):
        """the MCL page's frame buffer as text rows (# = lit pixel), 64 rows of 128"""
        fb = self.call("bb_mcl_fb")
        d = bytes(self.uc.mem_read(fb, 1024))
        return ["".join("#" if d[y * 16 + (x >> 3)] >> (7 - (x & 7)) & 1 else "." for x in range(128)) for y in range(64)]
