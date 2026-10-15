"""MCL follows the Blackbox's own sequencer (milestone M5b) under Unicorn: the stamp reader, the line fit, the virtual input clocks, FA / FB / FC and the PLL behind them
(src/mcl_bb/bb_clock.cpp, bb_clock_sync_seq) with the stamps written the way src/seqfix.c writes them.

Real code runs for: the whole timer interrupt (stamp reader, fit, transport state, PLL, tick emission), the TX ring and the USART6 wrapper.
Modelled in Python: the cycle counter and the timer (as test_mcl_sync.py) and the sequencer: audio blocks of 128 frames at 48 kHz, the player called a random
0..400 us after each block starts, with the sequencer time (960 ticks a beat) of the block start.
NOT checked (hardware only): the real stamps' jitter, what the stock sequencer does when stopped / located / looped, the audio output delay (set with bb_clock_seq_offset_us).
"""
import random
import statistics
import struct
import sys

from mcl_rig import MGR, clock_rig

CORE = 399_000_000
CYC_US = CORE / 1e6
LAT = 200
BLOCK = CORE * 128 / 48000                                       # cycles per audio block
STAMP = 0x38800940
fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


class Seq:
    """The modelled stock sequencer: piecewise-linear sequencer time against audio time; stamps like seqfix.c's."""

    def __init__(self):
        self.r = r = clock_rig(core_hz=CORE, tim_hz=200_000_000)
        r.call("bb_clock_init")
        r.st["step"] = 0
        r.call("bb_midi_claim", 1)
        r.e.w32(MGR + 0x20D - 3, 0x01000000)
        self.cpc = CORE / 1e6
        self.out = []                                            # (time, byte)
        self.ver = 0
        self.stamps = []                                         # (call time, now) of what we wrote
        self.tim_next = None
        self.t = 0

    def stamp(self, t, seq, now):
        e = self.r.e
        self.ver += 2
        e.w32(STAMP, self.ver | 1)
        e.w32(STAMP + 4, seq)
        e.w32(STAMP + 8, int(t) & 0xFFFFFFFF)
        e.w32(STAMP + 12, now & 0xFFFFFFFF)
        e.w32(STAMP + 16, (now >> 32) & 0xFFFFFFFF)
        e.w32(STAMP, self.ver)
        self.stamps.append((t, now))

    def start(self, t0=1_000_000):
        self.t = t0
        self.r.st["dwt"] = t0
        self.r.e.w32(STAMP, 0)
        self.r.call("bb_clock_sync_seq")
        self.tim_next = t0 + (self.r.e.r32(0x40001000 + 0x2C) + 1) * self.cpc

    def tim_arr(self):
        return (self.r.e.r32(0x40001000 + 0x2C) & 0xFFFF) + 1

    def run(self, events, t_end):
        """events: sorted list of (time, seq, now) stamps."""
        r = self.r
        events = list(events)
        while True:
            ti = events[0][0] if events else 1 << 62
            tt = self.tim_next
            t = min(ti, tt)
            if t > t_end:
                break
            if ti <= tt:
                _, sq, now = events.pop(0)
                r.st["dwt"] = int(t) & 0xFFFFFFFF
                self.stamp(t, sq, now)
            else:
                r.st["dwt"] = int(t + LAT) & 0xFFFFFFFF
                n0 = len(r.sent)
                r.call("mcl_tim6_isr")
                r.pump()
                for b in r.sent[n0:]:
                    self.out.append((t, b))
                self.tim_next = t + self.tim_arr() * self.cpc
        self.t = t_end

    def bytes(self, which, since=0):
        return [t for t, b in self.out if b == which and t >= since]


def blocks(bpm, t_start, seconds, now0=0, seq=0x2000, jitter_us=400, seed=1, step_noise=0):
    """stamps of a sequence playing at `bpm` (960 ticks a beat) from audio time t_start; the player is called 0..jitter_us after each block starts."""
    rnd = random.Random(seed)
    tps = bpm * 960 / 60 / CORE                                  # ticks per cycle
    ev, k = [], 0
    while k * BLOCK < seconds * CORE:
        tb = t_start + k * BLOCK
        now = now0 + round(k * BLOCK * tps)
        ev.append((tb + rnd.uniform(0, jitter_us) * CYC_US, seq, now))
        k += 1
    return ev


def model_clock_times(bpm, t_start, seconds, now0=0, mean_call_us=200):
    """instants (cycles) the sequencer time passes every 40th tick, plus the mean delay of the player's call after the block start"""
    cps = bpm * 960 / 60 / CORE
    out, k = [], -(-now0 // 40)
    while True:
        t = t_start + (k * 40 - now0) / cps + mean_call_us * CYC_US
        if t > t_start + seconds * CORE:
            return out
        out.append(t)
        k += 1


def nearest_offsets_us(ours, model):
    res = []
    for t in ours:
        res.append((t - min(model, key=lambda m: abs(m - t))) / CYC_US)
    return res


# --- 1. a sequence starts from the top at 120 BPM
s = Seq()
s.start()
T0 = 5_000_000
s.run(blocks(120, T0, 12), T0 + 12 * CORE)
check("locked and sending after the start", s.r.call("bb_clock_sync_state") == 3 and s.r.call("bb_clock_running") == 1, s.r.call("bb_clock_sync_state"))
check("transport reads running, stamps counted", s.r.call("bb_clock_seq_stat", 2) == 1 and s.r.call("bb_clock_seq_stat", 0) > 2000, (s.r.call("bb_clock_seq_stat", 2), s.r.call("bb_clock_seq_stat", 0)))
fa = s.bytes(0xFA)
check("exactly one FA (a start from the top), no FB, no FC", len(fa) == 1 and not s.bytes(0xFB) and not s.bytes(0xFC), (len(fa), len(s.bytes(0xFB)), len(s.bytes(0xFC))))
bpm = s.r.call("bb_clock_bpm_x10")
check("tempo read back: 120.0 BPM (within 0.1)", abs(bpm - 1200) <= 1, bpm)
f8 = s.bytes(0xF8, T0 + 4 * CORE)
off = nearest_offsets_us(f8, model_clock_times(120, T0, 12))
iv = [(b - a) / CYC_US for a, b in zip(f8, f8[1:])]
check("our F8 are at the sequencer's clock instants (+ the player's mean call delay) within 100 us (the stamps jitter +-115 us sd, the line averages it): worst %.1f, mean %.1f" % (max(map(abs, off)), statistics.mean(off)), max(map(abs, off)) < 100, max(map(abs, off)))
check("interval mean 20,833.3 us, sd %.1f us (the stamps have +-115 us of jitter)" % statistics.pstdev(iv), abs(statistics.mean(iv) - 20833.3) < 1 and statistics.pstdev(iv) < 15, (statistics.mean(iv), statistics.pstdev(iv)))
k = len(s.bytes(0xF8))
check("clock count follows the sequencer (about 12 s x 48 a second)", abs(k - 12 * 48) < 80, k)

# --- 1b. how steady the tempo reads (the screen shows it in tenths of a BPM): sampled every 100 ms over 8 s, with the stamps jittering +-115 us (sd) or 3x that
for jit in (400, 1200):
    s = Seq()
    s.start()
    ev = blocks(120, T0, 12, jitter_us=jit)
    reads = []
    for k in range(1, 81):
        s.run([x for x in ev if s.t < x[0] <= T0 + (2 + k / 10) * CORE], T0 + (2 + k / 10) * CORE)
        if k > 20:
            reads.append(s.r.call("bb_clock_bpm_x10"))
    check("stamp jitter %d us: tempo reads between %d and %d (tenths of a BPM) over 6 s" % (jit, min(reads), max(reads)), max(reads) - min(reads) <= 2 and abs(statistics.mean(reads) - 1200) <= 1, (min(reads), max(reads)))

# --- 2. stop (the player stops being called), start again from the top, then pause and continue
s = Seq()
s.start()
ev = blocks(120, T0, 5)
t_stop = T0 + 5 * CORE
ev += blocks(120, t_stop + 2 * CORE, 4, now0=0, seed=2)           # stopped 2 s, then from the top again
s.run(ev, T0 + 11 * CORE)
fc = s.bytes(0xFC)
check("stop: one FC, within 100 ms of the last stamp", len(fc) == 1 and 0 <= (fc[0] - t_stop) / CYC_US / 1000 < 100, [(x - t_stop) / CYC_US / 1000 for x in fc])
check("start again from the top: a second FA", len(s.bytes(0xFA)) == 2 and not s.bytes(0xFB), (len(s.bytes(0xFA)), len(s.bytes(0xFB))))
check("no clocks while stopped", not [t for t in s.bytes(0xF8) if fc[0] + 5e6 < t < t_stop + 2 * CORE], len([t for t in s.bytes(0xF8) if fc[0] + 5e6 < t < t_stop + 2 * CORE]))
s = Seq()
s.start()
ev = blocks(120, T0, 4)
tp = T0 + 4 * CORE
ev += blocks(120, tp + 3 * CORE, 4, now0=7680, seed=3)            # paused at 8 beats, continues from there
s.run(ev, T0 + 10.8 * CORE)                                        # (the stamps end at 11 s: stopping again after that is right, not part of this check)
check("pause and continue from beat 8: FA once, FC once, then FB", len(s.bytes(0xFA)) == 1 and len(s.bytes(0xFC)) == 1 and len(s.bytes(0xFB)) == 1, (len(s.bytes(0xFA)), len(s.bytes(0xFC)), len(s.bytes(0xFB))))
order = [b for t, b in s.out if b in (0xFA, 0xFB, 0xFC)]
check("... in that order", order == [0xFA, 0xFC, 0xFB], [hex(b) for b in order])
fb = s.bytes(0xFB)[0]
next_f8 = min(t for t in s.bytes(0xF8) if t > fb)
check("FB leaves with a clock (before the next one, no later than one period)", 0 <= (next_f8 - fb) / CYC_US < 21000, (next_f8 - fb) / CYC_US)

# --- 3. the stock sequencer keeps calling the player but the time stands still (a stop that does not stop the calls)
s = Seq()
s.start()
ev = blocks(120, T0, 4)
last = ev[-1][2]
tq = T0 + 4 * CORE
ev += [(tq + k * BLOCK + 100 * CYC_US, 0x2000, last) for k in range(300)]
s.run(ev, tq + 300 * BLOCK)
check("time standing still = stopped: one FC", len(s.bytes(0xFC)) == 1, len(s.bytes(0xFC)))

# --- 4. a tempo change 120 -> 90 BPM at beat 16 (the time keeps going, the slope changes)
s = Seq()
s.start()
ev = blocks(120, T0, 8)
t1 = T0 + 8 * CORE
n1 = round(8 * CORE * 120 * 960 / 60 / CORE)
ev += blocks(90, t1, 8, now0=n1, seed=4)
s.run(ev, t1 + 8 * CORE)
bpm = s.r.call("bb_clock_bpm_x10")
f8 = s.bytes(0xF8, t1 + 4 * CORE)
iv = [(b - a) / CYC_US for a, b in zip(f8, f8[1:])]
check("tempo change to 90 BPM is followed (reads %.1f), intervals 27,777 us" % (bpm / 10), abs(bpm - 900) <= 3 and abs(statistics.mean(iv) - 27777.8) < 5, (bpm, statistics.mean(iv)))
check("a tempo change is not a transport event", len(s.bytes(0xFA)) == 1 and not s.bytes(0xFC) and not s.bytes(0xFB))

# --- 5. a locate (the time jumps by 40 beats while running): the fit starts again, the clock keeps going, no transport bytes
s = Seq()
s.start()
ev = blocks(120, T0, 6)
t1 = T0 + 6 * CORE
ev += blocks(120, t1, 6, now0=round(6 * 120 * 960 / 60) + 38400, seed=5)
s.run(ev, t1 + 6 * CORE)
check("locate: restarts counted, still running", s.r.call("bb_clock_seq_stat", 1) >= 1 and s.r.call("bb_clock_seq_stat", 2) == 1, (s.r.call("bb_clock_seq_stat", 1), s.r.call("bb_clock_seq_stat", 2)))
check("locate: no extra FA / FB / FC, and the clock never paused for more than 100 ms", len(s.bytes(0xFA)) == 1 and not s.bytes(0xFC) and not s.bytes(0xFB), (len(s.bytes(0xFA)), len(s.bytes(0xFC)), len(s.bytes(0xFB))))
f8 = s.bytes(0xF8)
gaps = [(b - a) / CYC_US for a, b in zip(f8, f8[1:])]
check("locate: largest gap between clocks %.1f ms" % (max(gaps) / 1000), max(gaps) < 100000, max(gaps))

# --- 6. two sequences calling the player with different times: the first one seen is followed
s = Seq()
s.start()
a = blocks(120, T0, 6, seq=0x2000, seed=6)
b = blocks(120, T0 + 1_000_000, 6, now0=5000, seq=0x3000, seed=7)
s.run(sorted(a + b), T0 + 6 * CORE)
f8 = s.bytes(0xF8, T0 + 3 * CORE)
iv = [(y - x) / CYC_US for x, y in zip(f8, f8[1:])]
off = nearest_offsets_us(f8, model_clock_times(120, T0, 6))
check("a second sequence with another time is ignored: intervals steady (sd %.1f us), phase of the first (worst %.1f us)" % (statistics.pstdev(iv), max(map(abs, off))), statistics.pstdev(iv) < 15 and max(map(abs, off)) < 100, (statistics.pstdev(iv), max(map(abs, off))))

# --- 7. stale stamp left in backup SRAM from before (a cycle count of the last run): ignored, nothing sent
s = Seq()
s.r.e.w32(STAMP, 6)
s.r.e.w32(STAMP + 4, 0x2000)
s.r.e.w32(STAMP + 8, 1_000_000 - 3 * CORE & 0xFFFFFFFF)
s.r.e.w32(STAMP + 12, 4000)
s.r.e.w32(STAMP + 16, 0)
s.t = 1_000_000
s.r.st["dwt"] = 1_000_000
s.r.call("bb_clock_sync_seq")
s.tim_next = 1_000_000 + 1000 * s.cpc
s.run([], 1_000_000 + CORE // 2)
check("a stamp from long ago is not a running sequencer: nothing sent, listening", not s.out and s.r.call("bb_clock_sync_state") == 1, (s.out[:3], s.r.call("bb_clock_sync_state")))

# --- 8. offset: +2 ms moves our clocks 2 ms later
s = Seq()
s.start()
s.r.call("bb_clock_seq_offset_us", 2000)
s.run(blocks(120, T0, 10), T0 + 10 * CORE)
f8 = s.bytes(0xF8, T0 + 4 * CORE)
off = nearest_offsets_us(f8, model_clock_times(120, T0, 10))
check("offset +2000 us: our clocks are %.0f us later than without it" % statistics.mean(off), abs(statistics.mean(off) - 2000) < 60, statistics.mean(off))

# --- 9. the jack is ignored in this mode
s = Seq()
s.start()
for _ in range(10):
    s.r.e.call(s.r.b["mcl_rx_tap"], MGR, 0xF8)
s.run([], T0)
check("clocks from the jack are not used while following the sequencer", s.r.call("bb_clock_sync_state") == 1)

print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
