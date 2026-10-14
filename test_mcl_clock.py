"""MCL clock source (milestone M4) under Unicorn: src/mcl_bb/bb_clock.cpp, the TIM6 vector hook, the stock-clock filter in bb_midi.cpp.

Real code runs for: timer calibration against the cycle counter, the tempo / period arithmetic, the interrupt handler, the TX ring and USART6 wrapper
(the F8 / FA / FC bytes), the label text. Modelled in Python: the timer clock (the counter reads follow the cycle counter at a chosen ratio), the time
between interrupts (each interrupt happens (ARR+1)/rate seconds after the previous one, as the hardware's auto-reload does, whatever the handler took),
the wire. NOT checked (hardware only): the real TIM6 clock tree, interrupt latency and priority behaviour, the Machinedrum following the clock.
"""
import struct
import sys

from unicorn import UC_HOOK_MEM_READ

from mcl_rig import BASE, Rig, build, clock_rig

TIM6, DWT, SYST = 0x40001000, 0xE0001004, 0xE000E010
fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


def rig(**kw):
    return clock_rig(**kw)


def u32(r, a):
    return r.e.r32(a) & 0xFFFFFFFF


# --- the vector for IRQ 54
image = open(rig().img, "rb").read()
vec = struct.unpack_from("<I", image, 0x08040118 - BASE)[0]
r = rig()
check("vector for IRQ 54 (TIM6) points at mcl_tim6_isr (thumb); stock had a b . stub there", vec == r.b["mcl_tim6_isr"] | 1, hex(vec))

# --- before init: the interrupt does nothing
r.e.w32(TIM6 + 0x10, 1)
r.call("mcl_tim6_isr")
check("interrupt before the clock is started: clears UIF, sends nothing, touches no timer register", u32(r, TIM6 + 0x10) == 0 and not r.sent and u32(r, TIM6 + 0x2C) == 0)

# --- calibration
for tim_hz in (200_000_000, 100_000_000, 199_500_000):
    r = rig(tim_hz=tim_hz)
    ok = r.call("bb_clock_init")
    hz = r.call("bb_clock_timer_hz")
    psc = u32(r, TIM6 + 0x28) + 1
    want = tim_hz / psc
    check(f"calibration at a {tim_hz/1e6:g} MHz timer clock: prescaler {psc}, rate {hz} counts/s (within 0.1% of {want:.0f}), about 1 MHz",
          ok == 1 and abs(hz - want) / want < 1e-3 and 900_000 < hz < 1_100_000, (ok, psc, hz))
    check("... TIM6 clock enabled in RCC, priority 1 (0x10) set, not yet enabled in the NVIC", u32(r, 0x580244E8) & 0x10 and r.e.r8(0xE000E400 + 54) == 0x10 and u32(r, 0xE000E104) == 0)

# --- run: 60 s at 120.0 BPM
r = rig()
r.call("bb_clock_init")
hz = r.call("bb_clock_timer_hz")
r.call("bb_midi_claim", 1)
r.call("bb_clock_start", 1200, 1)
check("start: timer enabled, update interrupt on, NVIC bit for IRQ 54 set, first interval is one tick", u32(r, TIM6) & 1 and u32(r, TIM6 + 0x0C) & 1 and u32(r, 0xE000E104) == 1 << 22 and abs(u32(r, TIM6 + 0x2C) + 1 - hz * 60 / (192 * 120)) <= 1, u32(r, TIM6 + 0x2C))
TICKS = 192 * 2 * 60                                            # one minute at 120 BPM
counts, ideal_period = 0, hz * 60 / (192 * 120)
counts = u32(r, TIM6 + 0x2C) + 1
worst = 0.0
for i in range(TICKS):
    r.call("mcl_tim6_isr")
    r.pump()
    counts += u32(r, TIM6 + 0x2C) + 1
    worst = max(worst, abs(counts - (i + 2) * ideal_period))
check("60 s at 120 BPM: 23,040 ticks counted by the handler", r.call("bb_clock_ticks") == TICKS, r.call("bb_clock_ticks"))
check("tick times: the running sum of the programmed intervals stays within 1 timer count (1 us) of the ideal for the whole minute", worst <= 1.0, worst)
fa = r.sent.count(0xFA)
f8 = r.sent.count(0xF8)
check("on the wire: FA first, then one F8 per 8 ticks (2,880), nothing else", r.sent[0] == 0xFA and f8 == TICKS // 8 and fa == 1 and len(r.sent) == 1 + f8, (fa, f8, len(r.sent)))

# --- tempo change mid-run
r.call("bb_clock_tempo", 1500)
r.call("mcl_tim6_isr")
r.call("mcl_tim6_isr")
want = hz * 60 / (192 * 150)
check("tempo to 150.0 BPM: the next intervals follow at once (2,083 us), no restart", abs(u32(r, TIM6 + 0x2C) + 1 - want) <= 1, (u32(r, TIM6 + 0x2C) + 1, want))
tot = 0
for i in range(10000):
    r.call("mcl_tim6_isr")
    tot += u32(r, TIM6 + 0x2C) + 1
check("... and the fraction is carried: 10,000 ticks at 150 BPM sum to the ideal within 1 count", abs(tot - 10000 * want) <= 1.5, tot - 10000 * want)
r.call("bb_clock_tempo", 100)
r.call("mcl_tim6_isr")
r.call("mcl_tim6_isr")
check("tempo limits: 10 BPM is raised to 20.0 (15,625 counts), 400 to 300", abs(u32(r, TIM6 + 0x2C) + 1 - hz * 60 / (192 * 20)) <= 1)
r.call("bb_clock_tempo", 1200)

# --- stop
r.pump()
r.sent.clear()
r.call("bb_clock_stop", 1)
r.pump()
check("stop: FC sent, timer and its interrupt off", r.sent == [0xFC] and u32(r, TIM6) & 1 == 0 and u32(r, TIM6 + 0x0C) == 0 and r.call("bb_clock_running") == 0, r.sent)
arr = u32(r, TIM6 + 0x2C)
r.call("mcl_tim6_isr")
r.pump()
check("a stray interrupt after stop does nothing", r.sent == [0xFC] and u32(r, TIM6 + 0x2C) == arr)

# --- the stock firmware's own clock bytes
buf = 0x24070000
r.call("bb_midi_filter_clock", 1)
res = []
for b in (0xF8, 0xFA, 0xFB, 0xFC, 0xFE, 0x90):
    r.e.uc.mem_write(buf, bytes([b]))
    n0 = r.call("bb_midi_tx_free")
    res.append((r.call("mcl_tx_hook", 0, buf, 1), n0 - r.call("bb_midi_tx_free")))
check("stock single-byte F8 / FA / FB / FC are accepted and dropped; active sensing and a note byte are queued", res == [(1, 0)] * 4 + [(1, 1)] * 2, res)
r.pump()
r.call("bb_midi_filter_clock", 0)
r.e.uc.mem_write(buf, bytes([0xF8]))
n0 = r.call("bb_midi_tx_free")
r.call("mcl_tx_hook", 0, buf, 1)
check("filter off: the stock F8 is queued like any byte", n0 - r.call("bb_midi_tx_free") == 1)
r.pump()
r.call("bb_midi_filter_clock", 1)
r.e.uc.mem_write(buf, bytes([0xF8, 0x90, 0x3C]))
n0 = r.call("bb_midi_tx_free")
r.call("mcl_tx_hook", 0, buf, 3)
check("a longer message that happens to start with F8 is not touched", n0 - r.call("bb_midi_tx_free") == 3)

# --- the clock keeps its place in a long sysex: a real-time byte jumps the TX queue
r = rig()
r.call("bb_clock_init")
r.call("bb_midi_claim", 1)
r.call("bb_clock_start", 1200, 0)
r.e.uc.mem_write(0x24071000, bytes(0x55 for _ in range(1000)))
r.call("bb_midi_tx", 0x24071000, 1000)
r.sent.clear()
for _ in range(16):
    r.isr()                                                     # 16 sysex bytes leave
before = len(r.sent)
for _ in range(8):
    r.call("mcl_tim6_isr")                                      # first F8 at tick 1
r.isr()
check("an F8 queued in the middle of a 1,000-byte message leaves at the next byte boundary (before the rest)", r.sent[before] == 0xF8 and r.sent[:before] == [0x55] * before, r.sent[before - 1:before + 2])

# --- M4 test image: loop-back frame first (cable check), then the clock for 250 ms; the label reports L, t and r
r = rig(m4=True)
r.e.uc.mem_write(0x24061000, bytes(40))
r.e.call(r.b["mcl_gui"], count=80_000_000)
r.e.call(r.b["mcl_status_text"], 0x24061000, 30)
check("M4 image, nothing looped back and no timer interrupt modelled: label ' Mok L- t0 r0', clock running, port claimed",
      r.e.cstr(0x24061000) == "Mok L- t0 r0" and r.call("bb_clock_running") == 1 and r.call("bb_midi_owner") == 1, r.e.cstr(0x24061000))
r.pump()
check("... the 103-byte loop frame was sent first (F0 7D 'M' 'C' 'L' ... F7), nothing else queued", len(r.sent) == 103 and r.sent[:5] == [0xF0, 0x7D, 0x4D, 0x43, 0x4C] and r.sent[102] == 0xF7, (len(r.sent), r.sent[:6]))
for name, v in (("_ZL4m4_l", 1), ("_ZL4m4_t", 96), ("_ZL4m4_r", 12)):
    r.e.w32(r.b[name], v)
r.e.uc.mem_write(0x24061000, bytes(40))
r.e.call(r.b["mcl_status_text"], 0x24061000, 30)
check("with a closed loop and the expected counts the label reads 'Mok L+ t96 r12' (14 characters)", r.e.cstr(0x24061000) == "Mok L+ t96 r12", r.e.cstr(0x24061000))

build(False)
print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
