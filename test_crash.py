"""The crash recorder (src/crash.c, src/crash_thunk.S, patches/crash.py) under Unicorn.

Checked: a fault's frame, fault registers and breadcrumbs are recorded and survive into the next boot's record; a restart with
no fault (the watchdog) still keeps that run's breadcrumbs, uptime and free-RAM low point; the allocator's out-of-memory halt
records the size and the caller; the reset flags are read and cleared; the text lines; the patched vectors and call site.
NOT checked: a real fault on the unit (the handler's own stack switch runs only there), the reset itself (stubbed).
"""
import struct
import subprocess
import sys

from emu import Emu

IMG = "out/solo+slice+duck+chord+cond+filter+cpu+od+comp+seqfix+fx2+munchi+looper+lira+cave/BLACKBOX.bin"
e = Emu(IMG, "out/cave.elf")
e.uc.mem_map(0xC0000000, 0x400000)
e.uc.mem_map(0x38800000, 0x1000)
e.uc.mem_map(0x58024000, 0x1000)
e.uc.mem_map(0xE0000000, 0x10000)
fails = 0


def check(name, ok, info=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + ("  " + str(info) if info != "" else ""))
    fails += not ok


resets = []
e.stub(e.sym["crash_reset"], lambda: resets.append(1))
e.stub(e.sym["crash_ipsr"], value=0)                              # (Unicorn has no M-profile special registers)
e.stub(e.sym["crash_irq_off"], value=0)
e.stub(e.sym["crash_irq_on"])
e.w32(0x24000088, 0xC0100000)                                   # the allocator: 1 MB free
e.w32(0x2400008C, 0xC0000000)
RSR = 0x580244D0
REC = 0x38800B38
LIVE = 0x38800B20


from unicorn.arm_const import UC_ARM_REG_R0


def lines():
    out = []
    for i in range(24):
        buf = 0x2407F000
        e.call("crash_line", i, buf, 64)
        out.append(e.cstr(buf) if e.uc.reg_read(UC_ARM_REG_R0) else "")
    return out


# a first boot: power on
e.w32(RSR, 1 << 23)
e.call("crash_boot")
check("boot: the reset flags are cleared for the next restart (RMVF written)", e.r32(RSR) & (1 << 16), hex(e.r32(RSR)))
check("first boot: the record says power on, no fault", lines()[0] == "LAST RESTART: POWER ON / NO FAULT", lines()[:1])

# a run: blocks, free RAM going down, breadcrumbs
for i in range(64):
    if i == 32:
        e.w32(0x24000088, 0xC0040000)                           # 256 KB free for a while
    e.call("crash_tick")
e.w32(0x24000088, 0xC0080000)
for i in range(32):
    e.call("crash_tick")
e.uc.mem_write(0x38800B24, bytes([3]))                          # audio in SAMPLR
e.uc.mem_write(0x38800B25, bytes([11]))                         # gui painting

# a fault: a frame on a task stack
FR = 0x24070000
frame = [0x11, 0x22, 0x33, 0x44, 0x1212, 0x0805ABCD, 0x08123456, 0x21000000] + [0x080AAAA1, 0x080BBBB1, 0x080CCCC1, 0x080DDDD1]
e.uc.mem_write(FR, struct.pack("<12I", *frame))
e.w32(0xE000ED28, 0x00000400)                                    # CFSR: imprecise bus fault
e.w32(0xE000ED2C, 0x40000000)                                    # HFSR: forced
e.call("crash_record", FR, 0xFFFFFFFD, 3)
check("a fault resets the unit at once (no waiting for the watchdog)", resets == [1], resets)
e.w32(RSR, 1 << 24)                                              # what a SYSRESETREQ leaves
e.call("crash_boot")
L = lines()
check("next boot: SOFTWARE restart after a HARD FAULT", L[0] == "LAST RESTART: SOFTWARE / HARD FAULT", L[0])
check("... the faulting PC and LR", L[1] == "PC 08123456  LR 0805ABCD", L[1])
check("... the fault registers", L[2] == "CFSR 00000400 HFSR 40000000", L[2])
check("... the words above the frame (callers)", L[4] == "STK 080AAAA1 080BBBB1 080CCCC1", L[4])
check("... where our code was: audio in SAMPLR, GUI painting", L[5] == "IN: AUDIO SAMPLR GUI PAINT FILE -", L[5])
check("... the free-RAM low point and the last value", "RAM MIN 256 KB NOW 512" in L[6], L[6])
e.call("crash_pending")
check("a record not yet seen is pending (the footer shows RESTART)", e.uc.reg_read(UC_ARM_REG_R0) == 1)
e.call("crash_clear")
e.call("crash_pending")
check("SEEN / CLEAR: no longer pending", e.uc.reg_read(UC_ARM_REG_R0) == 0)

# a watchdog restart: no handler ran; the last run's breadcrumbs still say where our code was
for i in range(200):
    e.call("crash_tick")
e.uc.mem_write(0x38800B24, bytes([2, 0, 20]))                    # audio in DRONE, file task saving
e.w32(RSR, 1 << 26)
e.call("crash_boot")
L = lines()
check("watchdog restart: WATCHDOG / NO FAULT", L[0] == "LAST RESTART: WATCHDOG / NO FAULT", L[0])
check("... with the run's breadcrumbs (DRONE, SAVE)", L[5] == "IN: AUDIO DRONE GUI - FILE SAVE", L[5])
check("... and its uptime (200 blocks: 1 s)", L[6].startswith("RAN 1 S"), L[6])
e.call("crash_pending")
check("a watchdog restart is pending too", e.uc.reg_read(UC_ARM_REG_R0) == 1)

# out of memory: the allocator's halt
resets.clear()
e.call("crash_oom", 0, 0x20000, 0x0804C21B)
e.w32(RSR, 1 << 24)
e.call("crash_boot")
L = lines()
check("out of memory: recorded with the size asked for and the caller", L[0] == "LAST RESTART: SOFTWARE / OUT OF MEMORY" and "ASKED 131072" in L[7] and "LR 0804C21B" in L[1], (L[0], L[1], L[7]))
check("... and the unit restarts", resets == [1])
check("boots and faults counted", L[8].startswith("BOOTS 4  FAULTS 2"), L[8])

# a broken stack pointer: nothing is read from it
resets.clear()
e.call("crash_record", 0x60000000, 0xFFFFFFFD, 4)
e.w32(RSR, 1 << 24)
e.call("crash_boot")
L = lines()
check("a fault with a stack outside the RAM: recorded (MEMORY FAULT), the frame not read", L[0].endswith("MEMORY FAULT") and L[1] == "PC 00000000  LR 00000000", L[:2])

# the watchdog: the audio block's check and the two loops' stamps
OBJ = 0x24071000
checks = []
e.stub(0x08046994, lambda: checks.append(e.arg(0)))
e.w32(OBJ, 100)                                                   # stamp A at block 100, B at 300, now block 400
e.w32(OBJ + 4, 300)
e.w32(OBJ + 8, 400)
PER = 2560000                                                     # cycles a block (480 MHz, 256 frames at 48 kHz)
cyc = 1000
for i in range(40):
    cyc += PER
    e.w32(0xE0001004, cyc)
    e.call("crash_wd", OBJ)
check("the audio block's watchdog check still runs the stock one (the refresh)", len(checks) == 40 and checks[0] == OBJ, len(checks))
e.w32(0xE0001004, cyc + 64 * PER)                                 # the audio blocks stop; 64 blocks later (~341 ms) the GUI still stamps
e.call("crash_stamp_b", OBJ)
check("the GUI stamp still does the stock's job (B = blocks)", e.r32(OBJ + 4) == 400, e.r32(OBJ + 4))
e.w32(0xE0001004, cyc + 10 * PER)
e.call("crash_stamp_a", OBJ)
check("so does stamp A", e.r32(OBJ) == 400, e.r32(OBJ))
e.w32(RSR, 1 << 28)
e.call("crash_boot")
L = lines()
check("window watchdog restart: WINDOW WDG", L[0] == "LAST RESTART: WINDOW WDG / NO FAULT", L[0])
check("... and the lags at the last check (A 300 blocks, B 100)", L[10].startswith("LAG A 300 B 100  A AT "), L[10])

# loop A late: its saved PC / LR sampled from its TCB
TCB, STK = 0x24072000, 0x24072100
e.w32(0x2401F2A8, TCB)                                            # pxCurrentTCB while loop A stamps
e.w32(TCB, STK)                                                   # TCB: the saved stack pointer first
ctx = [0x2400BEEF] + [0] * 7 + [0xFFFFFFFD] + [0] * 5 + [0x0807ABC1, 0x08088757, 0x01000000]   # r4-r11, EXC_RETURN (no FPU), r0-r3 r12, lr, pc, xpsr
ctx += [0x24001234, 0x08074123, 0x00000000, 0x080740F9, 0x08074407, 0x0808A001, 0x08099999]  # the task's stack above: data and return addresses
e.uc.mem_write(STK, struct.pack("<24I", *ctx))
e.w32(TCB + 4, 0x00012345)                                        # its wake time
e.w32(0x2401F188, 0xFFFFFFFF)                                     # xNextTaskUnblockTime: damaged (never)
e.w32(0x2401F1A4, 0x00011111)                                     # xTickCount
e.w32(0x2401F1F0, 0x2401F208)                                     # pxDelayedTaskList
e.w32(0x2401F1EC, 0x2401F1F4)                                     # pxOverflowDelayedTaskList
e.w32(0x38800B28, 10000)                                          # (the live block's uptime: past the first 30 s)
e.w32(OBJ, 0); e.w32(OBJ + 4, 0); e.w32(OBJ + 8, 0)
e.call("crash_stamp_a", OBJ)
for blk in range(1, 520):
    e.w32(OBJ + 4, blk)                                           # the GUI keeps stamping
    e.w32(OBJ + 8, blk)
    cyc += PER
    e.w32(0xE0001004, cyc)
    e.call("crash_wd", OBJ)
e.w32(RSR, 1 << 28)
e.call("crash_boot")
L = lines()
check("loop A late: where it was (its saved PC and LR) and how many samples", any(l.startswith("A LATE AT PC 08088757 LR 0807ABC1 N 3") for l in L), L[11:])
check("... the object it waits on (r4) and its callers (odd code addresses above its frame)", L[14] == "OBJ 2400BEEF 08074123 080740F9 08074407" and L[15] == " 0808A001 08099999", L[14:16])
# in the first 30 s after a boot the loader is not sampled (it is late while the unit starts; the last run's samples stay)
e.w32(0x38800B28, 100)
n0 = e.r32(0x38800394)
e.w32(OBJ, 0); e.w32(OBJ + 8, 384)
e.call("crash_wd", OBJ)
check("no sample in the first 30 s after a boot", e.r32(0x38800394) == n0, (n0, e.r32(0x38800394)))

# the next wake-up later than the current delayed list's first task: brought back to it
DL = 0x2401F208
e.w32(DL, 1); e.w32(DL + 12, ITEM2 := 0x24072600); e.w32(ITEM2, 0x00012345)
e.w32(0x2401F188, 0xFFFFFFFF)
e.call("crash_wd", OBJ)
check("xNextTaskUnblockTime later than the first delayed task's wake time: set to it (the wait can end)", e.r32(0x2401F188) & 0xFFFFFFFF == 0x00012345, hex(e.r32(0x2401F188) & 0xFFFFFFFF))
e.w32(0x2401F188, 0x00010000)
e.call("crash_wd", OBJ)
check("... an earlier one is left alone", e.r32(0x2401F188) & 0xFFFFFFFF == 0x00010000)
e.w32(DL, 0)

# the RTOS clock: ticks a second and the scheduler held suspended
e.w32(OBJ, 0); e.w32(OBJ + 4, 0)
tick = 5000
for blk in range(1, 400):
    e.w32(OBJ + 8, blk)
    tick += 5 if blk < 200 else 0                                 # 5 ms a block, then the tick stops
    e.w32(0x2401F1A4, tick)
    e.w32(0x2401F180, 1 if blk >= 200 else 0)                     # and the scheduler is held suspended
    cyc += PER
    e.w32(0xE0001004, cyc)
    e.call("crash_wd", OBJ)
e.w32(RSR, 1 << 28)
e.call("crash_boot")
L = lines()
check("the tick rate (here: stopped in the last second) and the longest run of blocks with the scheduler suspended", L[18].startswith("TICKS/S 55 SUSPENDED MAX 200 NOW 200"), L[18])

# who has the CPU: the SysTick counts the running task; the three busiest of the last 250 ticks, named from their TCBs
for t, nm in ((0x24073000, b"audio"), (0x24073100, b"gui"), (0x24073200, b"usbmidi")):
    e.uc.mem_write(t + 0x34, nm + b"\0")
for t in [0x24073000] * 100 + [0x24073100] * 25 + [0x24073200] * 120 + [0x24074000] * 5:
    e.w32(0x2401F2A8, t)
    e.call("crash_systick")
e.w32(RSR, 1 << 28)
e.call("crash_boot")
L = lines()
check("the CPU shares of the last half second, by task name: audio 40 %, gui 10 %, usbmidi 48 %, the rest 2 %",
      L[20] == "CPU audio 40%  gui 10%  " and L[21] == "    usbmidi 48%  REST 2%", L[20:22])
for t in [0x24073100] * 250:                                      # the new run goes on: the record stays the last run's
    e.w32(0x2401F2A8, t)
    e.call("crash_systick")
L = lines()
check("... and they stay the last run's while the new run is measured", L[20] == "CPU audio 40%  gui 10%  ", L[20])
# the scheduler's bookkeeping: a ready list whose task is linked on it but with no bit in uxTopReadyPriority (or a wrong
# count) is put right
RL = 0x2401F21C + 20 * 3                                          # pxReadyTasksLists[3]
ITEM = 0x24072100 + 0x200                                         # the loader's state list item
def ready3(count, linked):
    end = RL + 8
    e.w32(RL, count)
    e.w32(RL + 4, end)
    e.w32(RL + 8, 0xFFFFFFFF)
    e.w32(RL + 12, ITEM if linked else end)                       # xListEnd.pxNext
    e.w32(RL + 16, ITEM if linked else end)
    e.w32(ITEM + 4, end)                                          # the item's pxNext: back to the end
e.w32(0x2401F1A0, 1 << 0)                                         # only the idle priority's bit
ready3(1, True)
e.call("crash_wd", OBJ)
check("a task linked on the priority-3 ready list and no bit: the bit is set (the task can run again)", e.r32(0x2401F1A0) & 0xFF == 0b1001, hex(e.r32(0x2401F1A0)))
L = lines()
check("... the repair is counted", "NOW 1 (PRIO 3)" in L[23], L[23])
e.call("crash_wd", OBJ)
check("... once (it stays right)", "NOW 1 (PRIO 3)" in lines()[23], lines()[23])
e.w32(0x2401F1A0, 1 << 0)
ready3(0, True)                                                   # the count says empty, the links say one
e.call("crash_wd", OBJ)
check("a linked task with the count at 0: count 1 and the bit back", e.r32(RL) == 1 and e.r32(0x2401F1A0) & 0xFF == 0b1001, (e.r32(RL), hex(e.r32(0x2401F1A0))))
ready3(0, False)
e.w32(0x2401F1A0, 1 << 0)
e.call("crash_wd", OBJ)
check("an empty list is left alone", e.r32(RL) == 0 and e.r32(0x2401F1A0) & 0xFF == 1)
e.w32(RSR, 1 << 28)
e.call("crash_boot")
check("at boot the last run's repairs are kept (2) and the new run's start at 0", "REPAIRS LAST 2 NOW 0" in lines()[23], lines()[23])
# the SD driver's read: a failed read is tried again (queue drained, HAL idle first), counted, kept at boot
e.uc.mem_map(0x52007000, 0x1000)
e.w32(0x52007034, 0x00000020)                                     # SDMMC STA: RX overrun
e.uc.mem_write(0x24009BFC + 0x30, bytes([1]))                     # HAL state READY
e.w32(0x24009BFC + 0x34, 0x20)                                    # its error code
tries = []
res_fail = [2]
def sd_read():
    tries.append(e.arg(2))
    e.ret(1 if len(tries) <= res_fail[0] else 0)
e.stubs[0x08045974] = sd_read
drained = []
e.stub(0x08087BE8, lambda: (drained.append(1), e.w32(e.arg(0), 0)))
e.stub(0x08087A5C, value=1000)
e.w32(0x2401F2A8, e.r32(0x38800390) & 0xFFFFFFFF)                # running in the loader's task (its TCB as seen at its stamps)
e.w32(0x2400A584, 10); e.w32(0x2400A58C, 5000)                    # the stock watchdog's {stamp A, stamp B, blocks}
e.call("crash_sd_read", 0, 0x24000000, 1234, 1)
check("a read that fails twice is tried again and works", e.uc.reg_read(UC_ARM_REG_R0) == 0 and tries == [1234] * 3 and len(drained) >= 2, (e.uc.reg_read(UC_ARM_REG_R0), tries))
check("... and a read that worked in the loader's task checks in for it with the stock watchdog (stamp A = now)", e.r32(0x2400A584) == 5000, e.r32(0x2400A584))
tries.clear(); res_fail[0] = 9
e.call("crash_sd_read", 0, 0x24000000, 77, 1)
check("... one that keeps failing gives up after three tries (an error, not a hang)", e.uc.reg_read(UC_ARM_REG_R0) == 1 and len(tries) == 3, tries)
e.w32(RSR, 1 << 28)
e.call("crash_boot")
L = lines()
check("the record: 5 failures, 1 read OK again, 1 given up, the HAL state / error and the SDMMC status", L[9] == "SD READ FAILS 5 OK AGAIN 1 GAVE UP 1 READS 1 01000020/00000020", L[9])

# a task on the ready list of another priority (the idle task on priority 5's list) is moved to its own list
def lst(pr):
    return 0x2401F21C + 20 * pr
def init_empty(pr):
    L0 = lst(pr)
    e.w32(L0, 0); e.w32(L0 + 4, L0 + 8); e.w32(L0 + 8, 0xFFFFFFFF); e.w32(L0 + 12, L0 + 8); e.w32(L0 + 16, L0 + 8)
for pr in range(7):
    init_empty(pr)
IDLE_TCB, IT = 0x24072800, 0x24072804                             # the idle task's TCB; its state list item at +4
e.w32(IDLE_TCB + 0x2C, 0)                                         # uxPriority 0
L5 = lst(5)
e.w32(IT + 0, 0); e.w32(IT + 4, L5 + 8); e.w32(IT + 8, L5 + 8); e.w32(IT + 12, IDLE_TCB); e.w32(IT + 16, L5)
e.w32(L5, 1); e.w32(L5 + 12, IT); e.w32(L5 + 16, IT)
e.w32(0x2401F1A0, 1 << 5)
e.call("crash_wd", OBJ)
L0 = lst(0)
check("the idle task found on priority 5's ready list is moved to priority 0's (counts, links, container, bits)",
      e.r32(L5) == 0 and e.r32(L5 + 12) == L5 + 8 and e.r32(L0) == 1 and e.r32(L0 + 12) == IT and e.r32(IT + 4) == L0 + 8 and e.r32(IT + 16) == L0
      and e.r32(0x2401F1A0) & 0x7F == 1, (e.r32(L5), hex(e.r32(L5 + 12)), e.r32(L0), hex(e.r32(L0 + 12)), hex(e.r32(0x2401F1A0))))
e.call("crash_wd", OBJ)
check("... and left there after", e.r32(L0) == 1 and e.r32(L5) == 0)
for pr in range(7):
    e.w32(lst(pr), 0); e.w32(lst(pr) + 12, 0)                     # (back to unset memory: the tests below do not set the lists up)

# the patch: the four fault vectors and the allocator's call site
img = open("out/solo+slice+duck+chord+cond+filter+cpu+od+comp+seqfix+fx2+munchi+looper+clkmon+lira+lirarec+lirasave+crash+cave/BLACKBOX.bin", "rb").read()
vec = struct.unpack("<4I", img[12:28])
check("vectors 3-6 (hard, memory, bus, usage) point at crash_fault", all(v == (e.sym["crash_fault"] | 1) for v in vec), [hex(v) for v in vec])
sys.path.insert(0, "patches")
from thumb import bl                                               # noqa: E402
o = 0x08044452 - 0x08040000
check("the allocator's halt message call goes to crash_oom_thunk", img[o:o + 4] == bl(0x08044452, e.sym["crash_oom_thunk"]))
check("SysTick (vector 15) goes through crash_systick_thunk", struct.unpack("<I", img[0x3C:0x40])[0] == (e.sym["crash_systick_thunk"] | 1))
check("the SD driver's read goes to the retrying wrapper with a 1000-tick wait; writes stay stock", img[0x080CFC24 - 0x08040000:0x080CFC2C - 0x08040000] == struct.pack("<2I", e.sym["crash_sd_read"] | 1, 0x080459E9) and img[0x08045998 - 0x08040000:][:4] == bytes.fromhex("40f2e832") and img[0x08045A0E - 0x08040000:][:4] == bytes.fromhex("41f28832"))

print("failures:", fails)
sys.exit(1 if fails else 0)
