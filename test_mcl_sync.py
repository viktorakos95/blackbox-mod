"""MCL clock follows a MIDI clock on the jack (milestone M5a) under Unicorn: the receive tap's stamping, the phase-locked loop and the timer interrupt in src/mcl_bb/bb_clock.cpp.

Real code runs for: mcl_rx_tap (stamping, chained to the real stock push), the PLL, mcl_tim6_isr, the TX ring and USART6 wrapper (the F8 / FA / FB / FC we send).
Modelled in Python: time (the cycle counter is set by the simulation to the instant of each event), the timer (an update event (ARR+1) counts after the previous one, as the
hardware reloads by itself; the interrupt is entered 0.5 us after the event), and the master (a MIDI clock with a chosen tempo, jitter, dropouts and transport bytes).
Measured: the instants our F8 leave (the interrupt that queues them; the wire adds the same few us every time) against the instants the master's arrive.
NOT checked (hardware only): the real interrupt latencies, the real masters' jitter shape, the stock firmware following our output.
"""
import random
import statistics
import struct
import sys

from mcl_rig import MGR, clock_rig

CORE = 399_000_000
CYC_US = CORE / 1e6
LAT = 200                                                        # interrupt entry latency, cycles (0.5 us)
fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


class Sim:
    def __init__(self):
        self.r = r = clock_rig(core_hz=CORE, tim_hz=200_000_000)
        r.call("bb_clock_init")
        assert abs(r.call("bb_clock_timer_hz") - 1_000_000) < 100
        r.st["step"] = 0
        self.t = 0
        self.cpc = CORE / 1e6                                    # cycles per timer count (PSC 200 of 200 MHz = 1 MHz)
        r.call("bb_midi_claim", 1)
        r.e.w32(MGR + 0x20D - 3, 0x01000000)
        self.out = []                                            # (event time in cycles, byte) of everything we queued for the wire
        self.in_events = []                                      # (t, byte) pending master bytes, sorted
        self.tim_next = None
        self.in_f8 = []

    def start(self, t0=1_000_000):
        self.t = t0
        self.r.st["dwt"] = t0
        self.r.call("bb_clock_sync_midi")
        self.tim_next = t0 + (self.r.e.r32(0x40001000 + 0x2C) + 1) * self.cpc

    def feed(self, events):
        self.in_events = sorted(self.in_events + events)

    def tim_arr(self):
        return (self.r.e.r32(0x40001000 + 0x2C) & 0xFFFF) + 1

    def run(self, t_end):
        r = self.r
        while True:
            ti = self.in_events[0][0] if self.in_events else 1 << 62
            tt = self.tim_next
            t = min(ti, tt)
            if t > t_end:
                break
            if ti <= tt:
                _, byte = self.in_events.pop(0)
                r.st["dwt"] = int(t) & 0xFFFFFFFF
                r.e.call(r.b["mcl_rx_tap"], MGR, byte)
                if byte == 0xF8:
                    self.in_f8.append(t)
            else:
                r.st["dwt"] = int(t + LAT) & 0xFFFFFFFF
                n0 = len(r.sent)
                r.call("mcl_tim6_isr")
                r.pump()
                for b in r.sent[n0:]:
                    self.out.append((t, b))
                self.tim_next = t + self.tim_arr() * self.cpc
        self.t = t_end

    def out_f8(self, since=0):
        return [t for t, b in self.out if b == 0xF8 and t >= since]


def master(bpm, t_start, seconds, sd_us=0.0, uniform_us=0.0, seed=1, skip=(), start_n=0):
    rnd = random.Random(seed)
    p = CORE * 60.0 / (24.0 * bpm)
    ev, n = [], 0
    while n * p < seconds * CORE:
        if (n + start_n) not in skip:
            j = rnd.gauss(0, sd_us) if sd_us else (rnd.uniform(-uniform_us, uniform_us) if uniform_us else 0.0)
            ev.append((t_start + n * p + j * CYC_US, 0xF8))
        n += 1
    return ev


def intervals_sd_us(times):
    iv = [(b - a) / CYC_US for a, b in zip(times, times[1:])]
    return statistics.pstdev(iv), statistics.mean(iv)


def offsets_us(sim, since):
    """our F8 minus the nearest master F8, microseconds"""
    ins = [t for t in sim.in_f8 if t >= since - 5e6]
    res = []
    last_in = max(sim.in_f8)
    for t in sim.out_f8(since):
        if t > last_in:                                          # after the master has ended: nothing to compare with
            continue
        near = min(ins, key=lambda x: abs(x - t))
        res.append((t - near) / CYC_US)
    return res


# --- 1. a clean 120 BPM clock: acquisition, lock, tempo, offset
s = Sim()
s.start()
check("listening: the interrupt polls every ~1 ms, nothing is sent", s.r.call("bb_clock_sync_state") == 1 and s.tim_arr() == 1000)
s.feed(master(120, 5_000_000, 12))
s.run(5_000_000 + 0.05 * CORE)
check("after two clocks the loop is locked and sending", s.r.call("bb_clock_sync_state") == 3 and s.r.call("bb_clock_running") == 1, s.r.call("bb_clock_sync_state"))
s.run(5_000_000 + 12 * CORE)
bpm = s.r.call("bb_clock_bpm_x10")
check("tempo read back: 120.0 BPM (within 0.1)", abs(bpm - 1200) <= 1, bpm)
off = offsets_us(s, 5_000_000 + 3 * CORE)
check("steady state, 9 s of a perfect clock: our F8 leaves within 10 us of the master's (mean %.2f us, worst %.2f us)" % (statistics.mean(off), max(map(abs, off))), max(map(abs, off)) < 10, max(map(abs, off)))
sd, mean = intervals_sd_us(s.out_f8(5_000_000 + 3 * CORE))
check("our F8 interval: mean 20,833.3 us, sd %.2f us" % sd, abs(mean - 20833.3) < 1 and sd < 2.0, (mean, sd))
check("error counters: largest phase error of the last second under 10 us, 500+ clocks processed", s.r.call("bb_clock_err_us", 0) < 10 and s.r.call("bb_clock_err_us", 2) > 500, (s.r.call("bb_clock_err_us", 0), s.r.call("bb_clock_err_us", 2)))
ticks = s.r.call("bb_clock_ticks")
check("192-PPQN tick counter runs at 8 per clock", abs(ticks - 8 * len(s.out_f8())) <= 8 + 8, (ticks, len(s.out_f8())))

# --- 2. jittery masters
for name, kw, factor in (("KeyStep-like, gaussian sd 35 us", dict(sd_us=35), 0.4), ("Machinedrum-like, gaussian sd 70 us", dict(sd_us=70), 0.4), ("a 1 ms software clock, uniform +-500 us", dict(uniform_us=500), 0.3)):
    s = Sim()
    s.start()
    s.feed(master(120, 5_000_000, 14, **kw))
    s.run(5_000_000 + 14 * CORE)
    sd_in, _ = intervals_sd_us(s.in_f8)
    sd_out, mean_out = intervals_sd_us(s.out_f8(5_000_000 + 4 * CORE))
    bpm = s.r.call("bb_clock_bpm_x10")
    check("%s: input interval sd %.1f us -> output %.1f us (under %d%% of it), tempo %.1f, mean interval right" % (name, sd_in, sd_out, factor * 100, bpm / 10),
          sd_out < factor * sd_in and abs(bpm - 1200) <= 2 and abs(mean_out - 20833.3) < 3, (sd_in, sd_out, bpm, mean_out))

# --- 3. tempo changes
s = Sim()
s.start()
ev = master(120, 5_000_000, 8)
t_change = 5_000_000 + 8 * CORE
ev += master(150, t_change, 10)
s.feed(ev)
s.run(t_change + 6 * CORE)
bpm = s.r.call("bb_clock_bpm_x10")
off = offsets_us(s, t_change + 4 * CORE)
check("tempo step 120 -> 150 BPM: followed within 6 s (reads %.1f), phase error back under 20 us" % (bpm / 10), abs(bpm - 1500) <= 2 and max(map(abs, off)) < 20, (bpm, max(map(abs, off))))
s = Sim()
s.start()
ev, tt, p0 = [], 5_000_000.0, None
for k in range(1100):                                            # a ramp from 100 to 140 BPM over ~25 s
    bpmk = 100 + 40 * min(k / 1000, 1)
    ev.append((tt, 0xF8))
    tt += CORE * 60 / (24 * bpmk)
s.feed(ev)
s.run(tt)
off = offsets_us(s, 5_000_000 + 2 * CORE)
check("tempo ramp 100 -> 140 BPM: the output stays within 800 us of the master all the way (worst %.0f us: the lag of a second-order loop on a ramp)" % max(map(abs, off)), max(map(abs, off)) < 800, max(map(abs, off)))

# --- 4. dropouts and slips
s = Sim()
s.start()
s.feed(master(120, 5_000_000, 20, skip={300}))
s.run(5_000_000 + 20 * CORE)
off = offsets_us(s, 5_000_000 + 12 * CORE)
check("one missing clock: no glitch (still locked, phase within 10 us)", s.r.call("bb_clock_sync_state") == 3 and max(map(abs, off)) < 10, max(map(abs, off)))
s = Sim()
s.start()
s.feed(master(120, 5_000_000, 20, skip=set(range(300, 306))))
s.run(5_000_000 + 12 * CORE)
st = s.r.call("bb_clock_sync_state")
s.run(5_000_000 + 20 * CORE)
off = offsets_us(s, 5_000_000 + 16 * CORE)
check("six missing clocks: the tempo is held, the loop is locked again after them and the phase is right", s.r.call("bb_clock_sync_state") == 3 and max(map(abs, off)) < 15, (st, max(map(abs, off))))
s = Sim()
s.start()
ev = master(120, 5_000_000, 6) + master(120, 5_000_000 + 6 * CORE + 0.0071 * CORE, 14)         # a 7.1 ms phase jump (a locate / a new run)
s.feed(ev)
s.run(5_000_000 + 12 * CORE)
off = offsets_us(s, 5_000_000 + 10 * CORE)
check("a 7 ms phase slip: re-locked within a few seconds, no leftover offset", max(map(abs, off)) < 15, max(map(abs, off)))
s = Sim()
s.start()
s.feed(master(120, 5_000_000, 5))
s.run(5_000_000 + 5.2 * CORE)
n_a = len(s.out_f8())
s.run(5_000_000 + 12 * CORE)
n_b = len(s.out_f8())
check("the master stops: held for 2 s of flywheel, then ours stops too and the loop listens again (state 1)", s.r.call("bb_clock_sync_state") == 1 and 85 < n_b - n_a < 125, (s.r.call("bb_clock_sync_state"), n_b - n_a))
s.feed(master(130, 5_000_000 + 13 * CORE, 6))
s.run(5_000_000 + 19 * CORE)
check("... and when it comes back at another tempo it is acquired afresh (130 BPM)", s.r.call("bb_clock_sync_state") == 3 and abs(s.r.call("bb_clock_bpm_x10") - 1300) <= 2, s.r.call("bb_clock_bpm_x10"))
s = Sim()
s.start()
s.feed(master(120, 5_000_000, 3) + [(5_000_000 + 0.0003 * CORE, 0xF8)])         # a stray clock a moment after the first, then silence
s.run(5_000_000 + 1 * CORE)
s.feed(master(120, 5_000_000 + 3 * CORE, 6, start_n=1000))
s.run(5_000_000 + 8 * CORE)
check("a stray clock 0.3 ms after the first (a 'tempo' of 5,000 BPM) is ignored; the tempo settles at 120 BPM", s.r.call("bb_clock_sync_state") == 3 and abs(s.r.call("bb_clock_bpm_x10") - 1200) <= 2, (s.r.call("bb_clock_sync_state"), s.r.call("bb_clock_bpm_x10")))

# --- 5. start / continue / stop on the jack are forwarded with the clock
s = Sim()
s.start()
ev = master(120, 5_000_000, 10)
tf = 5_000_000 + 4 * CORE - 3000
ev += [(tf, 0xFA), (5_000_000 + 7 * CORE - 3000, 0xFC), (5_000_000 + 8 * CORE - 3000, 0xFB)]
s.feed(ev)
s.run(5_000_000 + 10 * CORE)
cmds = [(t, b) for t, b in s.out if b in (0xFA, 0xFB, 0xFC)]
check("FA, FC, FB received are sent in the same order, once each", [b for _, b in cmds] == [0xFA, 0xFC, 0xFB], [hex(b) for _, b in cmds])
fa_t = cmds[0][0]
nxt = min((t for t in s.out_f8() if t >= fa_t - 1), default=None)
check("the FA leaves together with the next clock (within 50 us of it), not up to a clock early", nxt is not None and (nxt - fa_t) / CYC_US < 50, (nxt - fa_t) / CYC_US if nxt else None)
check("the 192-PPQN tick counter restarted at the FA", s.r.call("bb_clock_ticks") > 0)
n_f8_between = sum(1 for t, b in s.out if b == 0xF8 and cmds[1][0] <= t <= cmds[2][0])
check("the clock itself keeps running while stopped (a master that sends clock during stop)", n_f8_between > 40, n_f8_between)

# --- 6. internal mode untouched
s = Sim()
s.r.call("bb_clock_start", 1200, 1)
check("bb_clock_start after a sync run goes back to internal mode: state 0, running", s.r.call("bb_clock_sync_state") == 0 and s.r.call("bb_clock_running") == 1)
s.r.call("bb_clock_stop", 0)
check("bb_clock_stop ends it", s.r.call("bb_clock_running") == 0)

# --- the M5 hardware test image: boot-time lock and measurement, result on the label
r = clock_rig(core_hz=CORE, tim_hz=200_000_000, m5=True)
r.e.uc.mem_write(0x24061000, bytes(40))
r.e.call(r.b["bb_rt_gui"], count=120_000_000)
r.e.call(r.b["mcl_status_text"], 0x24061000, 30)
check("M5 image, no master on the jack: gui waits for the lock and reports it never locked, label ' Mok S1 0 e0'", r.e.cstr(0x24061000) == "Mok S1 0 e0" and r.call("bb_clock_sync_state") == 1, r.e.cstr(0x24061000))
for name, v in (("_ZL4m5_s", 3), ("_ZL4m5_b", 1200), ("_ZL4m5_e", 9)):
    r.e.w32(r.b[name], v)
r.e.uc.mem_write(0x24061000, bytes(40))
r.e.call(r.b["mcl_status_text"], 0x24061000, 30)
check("locked at 120.0 BPM with a 9 us worst phase error: label 'Mok S3 1200 e9' (14 characters)", r.e.cstr(0x24061000) == "Mok S3 1200 e9", r.e.cstr(0x24061000))
from mcl_rig import build
build(False)
print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
