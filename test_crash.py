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
e.stub(e.sym["crash_psp"], value=0)
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
check("... the GUI went on ~341 ms after the last audio block, loop A ~53 ms", L[9] == "AFTER AUDIO: GUI +341MS  LOOP A +53MS", L[9])
check("... and the lags at the last check (A 300 blocks, B 100)", L[10].startswith("LAG A 300 B 100  A AT "), L[10])

# loop A late: its saved PC / LR sampled from its TCB
TCB, STK = 0x24072000, 0x24072100
e.w32(0x2401F2A8, TCB)                                            # pxCurrentTCB while loop A stamps
e.w32(TCB, STK)                                                   # TCB: the saved stack pointer first
ctx = [0x2400BEEF] + [0] * 7 + [0xFFFFFFFD] + [0] * 5 + [0x0807ABC1, 0x08088757, 0x01000000]   # r4-r11, EXC_RETURN (no FPU), r0-r3 r12, lr, pc, xpsr
ctx += [0x24001234, 0x08074123, 0x00000000, 0x080740F9, 0x08074407, 0x0808A001, 0x08099999]  # the task's stack above: data and return addresses
e.uc.mem_write(STK, struct.pack("<24I", *ctx))
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
check("... the object it waits on (r4) and its callers (odd code addresses above the frame)", L[14] == "OBJ 2400BEEF CALLERS 08074123 080740F9" and L[15] == "08074407 0808A001 08099999", L[14:16])

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
check("... where (TCB of the running task at the longest one)", L[19].startswith("SUSP AT PC ") and L[19].endswith("TCB 24072000"), L[19])

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
# the loader starved: our engines give way
for lag, want in ((100, 0), (1300, 1), (2250, 2)):
    e.w32(OBJ, 0); e.w32(OBJ + 8, lag)
    e.call("crash_wd", OBJ)
    e.call("crash_starve")
    check("loader %d blocks late -> starve level %d" % (lag, want), e.uc.reg_read(UC_ARM_REG_R0) == want, e.uc.reg_read(UC_ARM_REG_R0))
e.w32(OBJ + 8, 0)
e.call("crash_wd", OBJ)

# the scheduler's bookkeeping: a ready list with tasks but no bit in uxTopReadyPriority gets its bit back
e.w32(0x2401F1A0, 1 << 0)                                         # only the idle priority's bit
e.w32(0x2401F21C + 20 * 3, 1)                                     # but a task (the loader) on the priority-3 ready list
e.w32(0x2401F21C + 20 * 6, 0)
e.stub(0x2400FFF0)                                                # (nothing)
e.call("crash_wd", OBJ)
check("a ready list with a task and no priority bit: the bit is set (the task can run again)", e.r32(0x2401F1A0) & 0xFF == 0b1001, hex(e.r32(0x2401F1A0)))
L = lines()
check("... and the repair is counted and shown", L[23].startswith("SCHED REPAIRS NOW 1 (PRIO 3)"), L[23])
e.call("crash_wd", OBJ)
L = lines()
check("... once (the bit stays)", L[23].startswith("SCHED REPAIRS NOW 1 (PRIO 3)"), L[23])

# the patch: the four fault vectors and the allocator's call site
img = open("out/solo+slice+duck+chord+cond+filter+cpu+od+comp+seqfix+fx2+munchi+looper+clkmon+lira+lirarec+lirasave+crash+cave/BLACKBOX.bin", "rb").read()
vec = struct.unpack("<4I", img[12:28])
check("vectors 3-6 (hard, memory, bus, usage) point at crash_fault", all(v == (e.sym["crash_fault"] | 1) for v in vec), [hex(v) for v in vec])
sys.path.insert(0, "patches")
from thumb import bl                                               # noqa: E402
o = 0x08044452 - 0x08040000
check("the allocator's halt message call goes to crash_oom_thunk", img[o:o + 4] == bl(0x08044452, e.sym["crash_oom_thunk"]))
check("SysTick (vector 15) goes through crash_systick_thunk", struct.unpack("<I", img[0x3C:0x40])[0] == (e.sym["crash_systick_thunk"] | 1))
check("the SD driver is the stock one again (table and waits untouched)", img[0x080CFC24 - 0x08040000:0x080CFC2C - 0x08040000] == struct.pack("<2I", 0x08045975, 0x080459E9) and img[0x08045998 - 0x08040000:][:4] == bytes.fromhex("41f28832"))

print("failures:", fails)
sys.exit(1 if fails else 0)
