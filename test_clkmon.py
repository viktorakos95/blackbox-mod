"""The MIDI clock monitor (src/clkmon.c, hooks patches/clkmon.py) under Unicorn, on the patched image.

Real code: the stock USART6 receive callback (0x08044e24) with the patched call into clkmon_rx and the stock byte push
behind it, the stock USB MIDI OUT callback (0x08044f1c) with clkmon_usb_rx, clkmon_boot, clkmon_block, clkmon_stats.
Stubbed: HAL_UART_Receive_IT and the USB endpoint re-arm. The cycle counter is a memory word the test sets.
Checked: ticks stamped on arrival, all bytes still reach the stock ring, intervals / jitter / latency against numbers worked
out here, the core clock found from the audio blocks, start / stop counters, USB packets, nothing touched while the backup
SRAM is off, a stale block from a previous boot cleared, reset.

python3 patch.py solo slice duck chord cond filter cpu od comp seqfix fx2 munchi looper clkmon cave && python3 test_clkmon.py
"""
import os
import random
import struct
import subprocess

from emu import Emu

IMG = "out/solo+slice+duck+chord+cond+filter+cpu+od+comp+seqfix+fx2+munchi+looper+clkmon+cave/BLACKBOX.bin"
MGR, HUART, RXBUF = 0x24002E88, 0x24009914, 0x24009910
USBBUF = 0x24009710
ISR = 0x38800988
CYC = 0xE0001004
RCC, PWR = 0x580244E0, 0x58024800
MEM, OUT = 0x30000000, 0x30008000
MHZ = 400.0
BLOCK = round(MHZ * 5333.3333)          # cycles per audio block
TICK = round(MHZ * 20833.3333)          # 120 BPM, 24 ppq

fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


os.makedirs("out/clk_test", exist_ok=True)
open("out/clk_test/off.c", "w").write(
    '#include <stdio.h>\n#include <stddef.h>\n#include "../../src/clkmon.h"\nint main(void){printf("%zu %zu %zu %zu %zu %zu %zu", '
    'sizeof(struct clkmon), sizeof(struct clk_stats), sizeof(struct clk_isr), offsetof(struct clkmon, reset), offsetof(struct clkmon, mhz), offsetof(struct clkmon, blk_min), offsetof(struct clkmon, blk_max));}\n')
subprocess.run(["gcc", "-o", "out/clk_test/off", "out/clk_test/off.c"], check=True)
SZ, STSZ, ISZ, O_RESET, O_MHZ, O_BMIN, O_BMAX = map(int, subprocess.run(["out/clk_test/off"], capture_output=True, text=True, check=True).stdout.split())
check("the interrupt block fits its backup SRAM window (0x38800988..0x38800a00)", ISZ <= 0x78, ISZ)

e = Emu(IMG, "out/cave.elf")
e.uc.mem_map(0x38800000, 0x1000)
e.uc.mem_map(0x58024000, 0x1000)
e.uc.mem_map(0xE0001000, 0x1000)
e.uc.mem_map(0xE000E000, 0x1000)
e.uc.mem_map(0x30000000, 0x100000)
e.stub(0x0808163C, value=0)                         # HAL_UART_Receive_IT
e.stub(0x080467CC)                                  # USB endpoint re-arm
e.uc.mem_write(MGR + 0x20D, b"\x01")                # the stock ring of port 0 is active
bkp_on = lambda on: (e.w32(RCC, (1 << 28) if on else 0), e.w32(PWR, (1 << 8) if on else 0))
cyc = lambda v: e.w32(CYC, v)
RVR = 0xE000E014                                     # SysTick reload: the firmware's 1 kHz tick from its core clock
e.w32(RVR, 400_000 - 1)


def rx(byte, t, port=0):
    cyc(t)
    e.uc.mem_write(RXBUF + port, bytes([byte]))
    e.call(0x08044E24, HUART + 0x8C * port, count=2_000_000)


def usb(packets, t):
    cyc(t)
    buf = b"".join(bytes(p) for p in packets).ljust(0x200, b"\0")
    e.uc.mem_write(USBBUF, buf)
    e.call(0x08044F1C, 0x24001000, count=2_000_000)


def block(t):
    cyc(t)
    e.call("clkmon_block", MEM, 128, count=2_000_000)


def stats(src):
    e.call("clkmon_stats", MEM, src, OUT, count=2_000_000)
    v = struct.unpack("<6I9fI", bytes(e.uc.mem_read(OUT, STSZ)))
    keys = "f8 fa fb fc over n bpm arr_min arr_max arr_sd stk_min stk_max stk_sd lat_avg lat_max cost".split()
    return dict(zip(keys, v))


def stock_ring():
    return e.r32(MGR + 0x204)                       # write index of port 0's ring


# ---- boot, with the backup SRAM still off: the monitor must not touch it
e.uc.mem_write(0x38800988, bytes([0xA5]) * 0x78)
e.uc.mem_write(MEM, bytes([0xFF]) * SZ)
e.call("clkmon_boot", MEM)
check("boot: the monitor state is cleared", bytes(e.uc.mem_read(MEM, SZ)) == bytes(SZ))
check("boot: the cycle counter is switched on (TRCENA, unlock, CYCCNTENA)",
      e.r32(0xE000EDFC) & (1 << 24) and e.r32(0xE0001FB0) & 0xFFFFFFFF == 0xC5ACCE55 and e.r32(0xE0001000) & 1)
e.uc.mem_write(0x38800988, bytes([0xA5]) * 0x78)    # a stale block from the previous boot / nothing yet
w0 = stock_ring()
rx(0xF8, 1000)
check("backup SRAM off: the hook leaves it alone", bytes(e.uc.mem_read(0x38800988, 0x78)) == bytes([0xA5]) * 0x78)
check("backup SRAM off: the byte still reaches the stock ring", stock_ring() == w0 + 1 and e.r8(MGR + w0 % 512) == 0xF8,
      (w0, stock_ring()))
rx(0x90, 1100)
check("... every other byte too", stock_ring() == w0 + 2 and e.r8(MGR + (w0 + 1) % 512) == 0x90)

# ---- backup SRAM on (as bkp_enable leaves it), the block cleared as t2m_boot does at the start of a boot
bkp_on(True)
e.uc.mem_write(0x38800988, bytes([0xA5]) * 0x78)
e.call("clkmon_boot", MEM)
check("boot: a stale interrupt block is cleared", bytes(e.uc.mem_read(ISR, ISZ)) == bytes(ISZ))
for b in (0x90, 0x3C, 0x7F, 0x80, 0xF0, 0xF7, 0xF9, 0xFD, 0xFE, 0xFF):      # not clock: no stamp (0xFE = active sensing)
    rx(b, 5000)
s = stats(0)
check("notes / sysex bytes are not counted as clock", s["f8"] == 0 and s["n"] == 0, s)

# ---- a steady 120 BPM clock with a little arrival jitter, the audio blocks free-running at the core clock
rng = random.Random(7)
N = 90
arr = [int(300_000 + i * TICK + rng.uniform(-900, 900)) for i in range(N)]       # up to ~2.5 us of byte-timing noise
blocks = [BLOCK * b + 12345 for b in range(1, int(arr[-1] / BLOCK) + 4)]
ev = sorted([(t, "a", i) for i, t in enumerate(arr)] + [(t, "b", 0) for t in blocks])
n_blk = n_arr = 0
for t, kind, _ in ev:
    if kind == "a":
        rx(0xF8, t)
        n_arr += 1
    else:
        block(t)
        n_blk += 1
s = stats(0)
# expected, worked out here
iv = [arr[i + 1] - arr[i] for i in range(N - 1)]
mean = sum(iv) / len(iv)
sd = (sum((x - mean) ** 2 for x in iv) / (len(iv) - 1)) ** .5
take = []                                            # the first block at or after each arrival
for a in arr:
    take.append(next(b for b in blocks if b >= a))
siv = [take[i + 1] - take[i] for i in range(N - 1)]
smean = sum(siv) / len(siv)
ssd = (sum((x - smean) ** 2 for x in siv) / (len(siv) - 1)) ** .5
lat = [b - a for a, b in zip(arr, take)]
us = lambda c: c / MHZ
close = lambda a, b, tol: abs(a - b) <= tol
check(f"{N} ticks counted", s["f8"] == N and s["n"] == N - 1, s)
check(f"core clock read from the SysTick reload: {MHZ} MHz", close(struct.unpack("<f", bytes(e.uc.mem_read(MEM + O_MHZ, 4)))[0], MHZ, 0.01))
check(f"audio block gaps seen: {BLOCK} cycles each", e.r32(MEM + O_BMIN) == BLOCK and e.r32(MEM + O_BMAX) == BLOCK, (e.r32(MEM + O_BMIN), e.r32(MEM + O_BMAX)))
check(f"tempo 120 BPM (got {s['bpm']:.2f})", close(s["bpm"], 120.0, 0.05), s["bpm"])
check(f"arrival interval min / max {us(min(iv)):.1f} / {us(max(iv)):.1f} us",
      close(s["arr_min"], us(min(iv)), 0.1) and close(s["arr_max"], us(max(iv)), 0.1), (s["arr_min"], s["arr_max"]))
check(f"arrival jitter sd {us(sd):.2f} us", close(s["arr_sd"], us(sd), 0.1), s["arr_sd"])
check(f"stock-view interval min / max {us(min(siv)):.0f} / {us(max(siv)):.0f} us (whole blocks)",
      close(s["stk_min"], us(min(siv)), 0.5) and close(s["stk_max"], us(max(siv)), 0.5), (s["stk_min"], s["stk_max"]))
check(f"stock-view jitter sd {us(ssd):.0f} us, far above the arrival jitter", close(s["stk_sd"], us(ssd), 2.0) and s["stk_sd"] > 100 * s["arr_sd"],
      (s["stk_sd"], ssd))
check(f"latency to the taking block: avg {us(sum(lat) / len(lat)):.0f} max {us(max(lat)):.0f} us",
      close(s["lat_avg"], us(sum(lat) / len(lat)), 1.0) and close(s["lat_max"], us(max(lat)), 1.0), (s["lat_avg"], s["lat_max"]))
check("the stock ring got every byte, clock included", stock_ring() == w0 + 2 + 10 + N, (stock_ring(), w0))
check("the interrupt cost is recorded (0 here: the counter is frozen between its two reads)", s["cost"] == 0)

# ---- start / continue / stop
for b, t in ((0xFA, 40_000_000), (0xFB, 40_001_000), (0xFC, 40_002_000), (0xFC, 40_003_000)):
    rx(b, t)
s = stats(0)
check("FA / FB / FC counted (1 / 1 / 2)", (s["fa"], s["fb"], s["fc"]) == (1, 1, 2), s)

# ---- a stop and restart is a gap, not an interval
block(41_000_000)
rx(0xF8, 41_500_000)
block(42_000_000)
n0 = stats(0)["n"]
rx(0xF8, 41_500_000 + TICK * 40)                    # 40 ticks later: a restart
block(41_500_000 + TICK * 40 + 5)
check("a pause longer than the gap limit adds no interval", stats(0)["n"] == n0, (n0, stats(0)["n"]))

# ---- overflow: more stamps than the ring holds between two blocks
for i in range(12):
    rx(0xF8, 60_000_000 + i * 1000)
block(60_100_000)
s = stats(0)
check("12 ticks in one block: 4 lost, counted", s["over"] == 4, s["over"])

# ---- USB: clock events inside event packets, other packets ignored, the stock copy still done
slot_before = e.r32(MGR + 0x5C74)
usb([[0x09, 0x90, 0x3C, 0x7F], [0x0F, 0xF8, 0, 0], [0x1F, 0xF8, 0, 0], [0x0B, 0xB0, 0x01, 0x40], [0x0F, 0xFA, 0, 0]], 70_000_000)
block(70_100_000)
s = stats(1)
check("USB: 2 ticks and a start in one packet buffer", (s["f8"], s["fa"], s["fb"], s["fc"]) == (2, 1, 0, 0), s)
check("USB: the stock ring write slot advanced (the stock copy ran)", e.r32(MGR + 0x5C74) == slot_before + 1,
      (slot_before, e.r32(MGR + 0x5C74)))
check("the jack's counters are untouched by USB", stats(0)["f8"] == N + 2 + 12 and stats(0)["fa"] == 1, stats(0))

# ---- the tempo follows a change within a beat (a cumulative mean would take minutes)
def run_ticks(n, interval, t0):
    t = t0
    for _ in range(n):
        t += interval
        rx(0xF8, t)
        block(t + 1000)
    return t


e.w32(MEM + O_RESET, 1)                             # a clean start: the ticks above were a test of overflow, not of tempo
block(95_000_000)
t = run_ticks(30, TICK, 100_000_000)
check("tempo 120.0 BPM after 30 ticks", close(stats(0)["bpm"], 120.0, 0.05), stats(0)["bpm"])
t = run_ticks(12, TICK * 2, t)
mid = stats(0)["bpm"]
check(f"12 ticks into a change to 60 BPM: between the two ({mid:.1f})", 60 < mid < 120, mid)
t = run_ticks(60, TICK * 2, t)
check(f"60 ticks after the change: 60.0 BPM ({stats(0)['bpm']:.2f})", close(stats(0)["bpm"], 60.0, 0.1), stats(0)["bpm"])
t = run_ticks(60, TICK // 2, t)
check(f"and 240 BPM after another change ({stats(0)['bpm']:.1f})", close(stats(0)["bpm"], 240.0, 0.3), stats(0)["bpm"])
block(t + 450_000_000)
check("no tick for a second: no tempo shown", stats(0)["bpm"] == 0.0, stats(0)["bpm"])
e.w32(RVR, 0)
block(t + 460_000_000)
check("a SysTick reload that is not a plausible core clock: CORE not shown (0)", struct.unpack("<f", bytes(e.uc.mem_read(MEM + O_MHZ, 4)))[0] == 0.0)
e.w32(RVR, 400_000 - 1)

# ---- reset
e.w32(MEM + O_RESET, 1)
block(80_000_000)
s0, s1 = stats(0), stats(1)
check("reset: counters and intervals start again", s0["f8"] == 0 and s0["n"] == 0 and s0["over"] == 0 and s0["fa"] == 0 and s1["f8"] == 0,
      (s0, s1))
rx(0xF8, 80_100_000)
rx(0xF8, 80_100_000 + TICK)
block(80_200_000)
s0 = stats(0)
check("after a reset it counts again (2 ticks, 1 interval; the tempo needs 4 intervals)", s0["f8"] == 2 and s0["n"] == 1 and s0["bpm"] == 0.0, s0)
rx(0xF8, 80_100_000 + 2 * TICK)
rx(0xF8, 80_100_000 + 3 * TICK)
rx(0xF8, 80_100_000 + 4 * TICK)
block(80_300_000 + 4 * TICK)
check("... and shows 120 BPM from the fourth interval on", close(stats(0)["bpm"], 120.0, 0.05), stats(0)["bpm"])

# ---- a sender whose own timer steps in 1 ms (the display must not flicker between neighbouring values at 240 BPM)
e.w32(MEM + O_RESET, 1)
block(90_000_000)
ms = 400_000
ticks = [200_000_000 + round(i * 10.4) * ms for i in range(1, 400)]                   # 240.385 BPM, each tick on a whole ms
shown, t_blk = [], 200_000_000
ev = sorted([(t, 1) for t in ticks] + [(200_000_000 + b * BLOCK // 2, 0) for b in range(1, ticks[-1] // (BLOCK // 2) - 99)])
last_naive, naive = None, []
seen = []
for t, kind in ev:
    if t > ticks[-1] + 1000:
        break
    if kind:
        rx(0xF8, t)
        seen.append(t)
        block(t + 1000)
        if len(seen) > 60:
            shown.append(stats(0)["bpm"])
            naive.append(60e6 * MHZ / (seen[-1] - seen[-25]))                           # the end-point estimate over one beat, for comparison
    else:
        block(t)
spread, nspread = max(shown) - min(shown), max(naive) - min(naive)
check(f"240.4 BPM from a 1 ms-stepped sender: shown tempo within {spread:.2f} BPM over {len(shown)} ticks (an end-point estimate wanders {nspread:.2f})",
      spread <= 0.35 and spread < nspread / 2 and close(sum(shown) / len(shown), 60e6 / (24 * 10400), 0.3), (spread, nspread, sum(shown) / len(shown)))
changes = sum(1 for a, b in zip(shown, shown[1:]) if a != b)
check(f"... and it changed {changes} times in {len(shown)} ticks (the end-point estimate would flip nearly every tick)", changes <= len(shown) // 10, changes)

# ---- a tick that arrives after the block read the clock must not look like it came from the future
e.w32(MEM + O_RESET, 1)
block(300_000_000)
rx(0xF8, 301_000_000)
block(300_500_000)                                  # the counter reads earlier than the stamp
s0 = stats(0)
check("a stamp newer than the block's clock gives latency 0, not 4 billion cycles", s0["lat_max"] == 0.0 and s0["lat_avg"] == 0.0, (s0["lat_avg"], s0["lat_max"]))

# ---- the stock sequencer's position against audio time (what a plugin host's position would be judged by)
SEQA, SEQB = 0x30020000, 0x30021000                     # two sequence objects (only their addresses matter)


def seq(lo, sq=SEQA):
    e.call("clkmon_seq", MEM, sq, lo & 0xFFFFFFFF, 0, count=2_000_000)


def xp():
    e.call("clkmon_xp_stats", MEM, OUT, count=2_000_000)
    v = struct.unpack("<IIii5f5I", bytes(e.uc.mem_read(OUT, 56)))
    return dict(zip("calls restarts dt_min dt_max tps res_min res_max res_sd fs stalls slips misses seqs spread".split(), v))


BLK_CYC = round(MHZ * 1e6 * 128 / 48000)               # cycles per 128-frame block at 48 kHz
e.w32(MEM + O_RESET, 1)
t = 500_000_000
block(t)
for k in range(1, 801):                                 # internal clock, 120 BPM: 20.48 ticks a block, whole ticks
    t += BLK_CYC
    block(t)
    seq(int(20.48 * (k - 1)) + rng.randint(0, 20))       # the player runs per chunk of the block: an early chunk ends somewhere ...
    seq(int(20.48 * k) - 5000, SEQB)                    # a second sequence, 5000 ticks behind: noted as spread, not followed
    seq(int(20.48 * k))                                 # ... and the block's last chunk ends at the block's end: this one counts
a = xp()
check(f"sample rate measured against the core clock: {a['fs']:.0f} Hz", close(a["fs"], 48000, 30), a["fs"])
check(f"internal clock: the position moves {a['tps']:.1f} ticks a second (20.48 a block)", close(a["tps"], 7680.0, 5.0), a["tps"])
check("... three calls a block seen, the second sequence's distance kept (5000 ticks), not mixed in", a["seqs"] == 3 and a["spread"] == 5000, a)
check("... no stalls, slips or missed blocks", (a["stalls"], a["slips"], a["misses"]) == (0, 0, 0), a)
check(f"... advances {a['dt_min']}-{a['dt_max']} ticks a block", (a["dt_min"], a["dt_max"]) == (20, 21), a)
check(f"... and sits on its line to within {a['res_min']:.0f} .. {a['res_max']:.0f} us (whole ticks: about +-65 plus the fit)", -110 < a["res_min"] and a["res_max"] < 110, a)

# a stop (no calls for a while) and a restart from 0: the window starts again, the numbers stay clean
for _ in range(20):
    t += BLK_CYC
    block(t)
for k in range(0, 100):
    t += BLK_CYC
    block(t)
    seq(int(20.48 * k))
b = xp()
check("a stop and a restart from position 0: one window restart, residuals still clean",
      b["restarts"] == a["restarts"] + 1 and -110 < b["res_min"] and b["res_max"] < 110, (a["restarts"], b))

# following an external clock: stock moves the position a whole tick (160) at the block that takes the F8, not smoothly
e.w32(MEM + O_RESET, 1)
t += BLK_CYC
block(t)
for k in range(1, 801):
    t += BLK_CYC
    block(t)
    seq(160 * int(k * 128 / 48000 / (60 / 120 / 24)))   # 120 BPM, a step of 160 ticks per F8
c_ = xp()
check(f"following 120 BPM: the position moves in steps ({c_['dt_min']}-{c_['dt_max']} ticks a block)", (c_["dt_min"], c_["dt_max"]) == (0, 160), c_)
check(f"... and strays {c_['res_min']:.0f} .. {c_['res_max']:.0f} us off the line (a staircase is far from a line)", c_["res_max"] - c_["res_min"] > 1500, c_)
check(f"... and its rate from a 32-block window is only roughly right ({c_['tps']:.0f} ticks a second)", close(c_["tps"], 7680.0, 400.0), c_["tps"])

# a stall: the time stands still for one block, then catches up (what the hardware showed: 0 then 10 ticks a block)
e.w32(MEM + O_RESET, 1)
t += BLK_CYC
block(t)
pos = 0
for k in range(1, 201):
    t += BLK_CYC
    block(t)
    if k != 120:
        pos = int(5.12 * k)
    seq(pos)
d = xp()
check(f"one stalled block is counted ({d['stalls']} stall, {d['slips']} slip, {d['res_min']:.0f} us off the line)", d["stalls"] == 1 and d["slips"] >= 1 and d["res_min"] < -2000, d)
# a block without a call for the followed sequence
t += BLK_CYC
block(t)
t += BLK_CYC
block(t)
seq(int(5.12 * 202))
t += BLK_CYC
block(t)
seq(int(5.12 * 203))                                    # (a block's point is taken when the next block starts)
check("a block with no call in between is counted as a miss", xp()["misses"] == 1, xp())

# ---- wiring: seq_play (src/seqfix.c) hands the monitor the musical time from the stock clock conversion, not the clock position
got = []
e.stub(e.sym["t2m_seq"], fn=lambda: got.append((e.arg(0), e.arg(1), e.arg(2))))
e.stub(0x08069C94, fn=lambda: e.uc.mem_write(e.arg(0), struct.pack("<q", 123456)))    # clock position -> musical time
e.stub(0x0805D1D8)                                                                        # the player itself
e.uc.mem_write(0x30010000, struct.pack("<q", 7_000_000))                                  # the clock: a sample position
e.call("seq_play", 0x30011000, 0x30010000, 128, 0, 0, count=2_000_000)
check("seq_play passes the sequence and its musical time to the monitor, not the sample position", got == [(0x30011000, 123456, 0)], got)

print(f"{fails} failure(s)")
raise SystemExit(1 if fails else 0)
