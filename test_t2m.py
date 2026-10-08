"""Trigger2MIDI: src/t2m.c against t2m_ref.py (the gen~ codebox transcribed line for line, double precision).

Synthetic drum signals (single hits over the velocity range, 16ths with accents, rolls, flams, ghost notes near the
threshold, a ringing tom, narrowband bleed, a noise floor, clipped hits) run through the reference and through the C
port, built twice: for this machine (all signals, many settings) and for the Cortex-M7 under Unicorn (the exact
instructions the Blackbox runs). Required: the same MIDI messages on the same samples.

python3 test_t2m.py            (needs gcc, arm-none-eabi-gcc, unicorn; no firmware image)
"""
import ctypes
import os
import random
import struct
import subprocess
import sys
from math import exp, pi, sin

import t2m_ref as ref

SR = 48000
BLOCK = 256
OUT = "out/t2m_test"
fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


# ---------------------------------------------------------------- builds
os.makedirs(OUT, exist_ok=True)
SHIM = OUT + "/shim.c"
open(SHIM, "w").write('#include <stddef.h>\n#include "../../src/t2m.h"\n'
                      "unsigned t2m_sizeof_state(void) { return sizeof(t2m_state); }\n"
                      "unsigned t2m_sizeof_params(void) { return sizeof(t2m_params); }\n"
                      "unsigned t2m_off_events(void) { return offsetof(t2m_state, events); }\n"
                      "unsigned t2m_off_n_events(void) { return offsetof(t2m_state, n_events); }\n")
subprocess.run(["gcc", "-O2", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off",
                "-o", OUT + "/t2m.so", "src/t2m.c", SHIM], check=True)
LD = OUT + "/t2m.ld"
open(LD, "w").write("MEMORY { F (rx) : ORIGIN = 0x08040000, LENGTH = 0x10000 }\n"
                    "SECTIONS { .text : { *(.text .text.* .rodata .rodata.*) } > F\n"
                    "  /DISCARD/ : { *(.data .data.* .bss .bss.* COMMON .ARM.exidx* .comment) } }\n")
CFLAGS = ["-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16", "-Os", "-ffreestanding", "-nostdlib",
          "-fno-builtin", "-Wall", "-Wextra", "-Werror"]
subprocess.run(["arm-none-eabi-gcc", *CFLAGS, "-T", LD, "-Wl,-e,0", "-o", OUT + "/t2m.elf", "src/t2m.c", SHIM],
               check=True)
subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", "-j", ".text", OUT + "/t2m.elf", OUT + "/t2m.bin"], check=True)
size = os.path.getsize(OUT + "/t2m.bin")
print(f"ARM code size: {size} bytes")


# ---------------------------------------------------------------- params
class Params(ctypes.Structure):
    _fields_ = [(n, ctypes.c_float) for n in
                ("sens", "thresh", "retrig", "mask_ms", "scan_ms", "curve", "strict_db", "speed_ms", "spec_strict",
                 "spec_range")] + [("len_ms", ctypes.c_uint16)] + [(n, ctypes.c_uint8) for n in
                ("note_on", "note", "vel_mode", "vel_fixed", "len_mode", "cc_on", "cc_num", "cc_mode", "cc_fixed",
                 "channel")]


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def make_params(d):
    """The C struct and the same values (floats rounded to float32) for the reference."""
    p = Params(**d)
    r = dict(d)
    for k, _ in Params._fields_[:10]:
        r[k] = f32(d[k])
    return p, r


# ---------------------------------------------------------------- runners
lib = ctypes.CDLL(os.path.abspath(OUT + "/t2m.so"))
STATE_SIZE = lib.t2m_sizeof_state()
OFF_EVENTS, OFF_N = lib.t2m_off_events(), lib.t2m_off_n_events()
check("params struct has the same layout in the test as in C", lib.t2m_sizeof_params() == ctypes.sizeof(Params),
      (lib.t2m_sizeof_params(), ctypes.sizeof(Params)))


def read_events(buf, base):
    n = struct.unpack_from("<I", buf, OFF_N)[0]
    out = []
    for k in range(n):
        frame, status, d1, d2 = struct.unpack_from("<HBBB", buf, OFF_EVENTS + 6 * k)
        out.append((base + frame, status, d1, d2))
    return out


def run_host(sig, p):
    st = ctypes.create_string_buffer(STATE_SIZE)
    lib.t2m_reset(st)
    ev = []
    for b in range(0, len(sig), BLOCK):
        chunk = sig[b:b + BLOCK]
        arr = (ctypes.c_float * len(chunk))(*chunk)
        lib.t2m_process(st, ctypes.byref(p), arr, len(chunk))
        ev += read_events(st.raw, b)
    return ev


def run_arm(sig, p):
    from emu import Emu
    e = Emu(OUT + "/t2m.bin", OUT + "/t2m.elf")
    ST, PR, IN = 0x24010000, 0x24011000, 0x24012000
    e.uc.mem_write(PR, bytes(p))
    e.call("t2m_reset", ST)
    ev = []
    for b in range(0, len(sig), BLOCK):
        chunk = sig[b:b + BLOCK]
        e.uc.mem_write(IN, struct.pack(f"<{len(chunk)}f", *chunk))
        e.call("t2m_process", ST, PR, IN, len(chunk), count=20_000_000)
        ev += read_events(bytes(e.uc.mem_read(ST, STATE_SIZE)), b)
    return ev


def count_instructions(sig, p):
    """Instructions per sample on the M7 build (a rough cycle figure: the M7 issues up to two a cycle)."""
    from emu import Emu
    from unicorn import UC_HOOK_CODE
    e = Emu(OUT + "/t2m.bin", OUT + "/t2m.elf")
    ST, PR, IN = 0x24010000, 0x24011000, 0x24012000
    e.uc.mem_write(PR, bytes(p))
    e.call("t2m_reset", ST)
    n = [0]
    e.uc.hook_add(UC_HOOK_CODE, lambda *a: n.__setitem__(0, n[0] + 1))
    for b in range(0, len(sig) - BLOCK + 1, BLOCK):
        e.uc.mem_write(IN, struct.pack(f"<{BLOCK}f", *sig[b:b + BLOCK]))
        e.call("t2m_process", ST, PR, IN, BLOCK, count=20_000_000)
    return n[0] / (len(sig) // BLOCK * BLOCK)


# ---------------------------------------------------------------- signals
def drum(rng, amp, body_hz=None, body_tau=None, noise_tau=None, attack_ms=None, n=None):
    body_hz = body_hz or rng.uniform(60, 250)
    body_tau = body_tau or rng.uniform(0.03, 0.15)
    noise_tau = noise_tau or rng.uniform(0.002, 0.012)
    attack = int((attack_ms if attack_ms is not None else rng.uniform(0.2, 2.0)) * SR / 1000) + 1
    n = n or int(SR * min(1.5, body_tau * 8))
    mix = rng.uniform(0.3, 0.8)
    ph = rng.uniform(0, 2 * pi)
    out = []
    for i in range(n):
        t = i / SR
        a = min(1.0, (i + 1) / attack)
        out.append(amp * a * ((1 - mix) * sin(ph + 2 * pi * body_hz * t) * exp(-t / body_tau)
                              + mix * rng.uniform(-1, 1) * exp(-t / noise_tau)))
    return out


def place(length, events, rng, floor=0.0):
    sig = [floor * rng.uniform(-1, 1) for _ in range(length)]
    for at, s in events:
        for i, x in enumerate(s):
            if at + i < length:
                sig[at + i] += x
    return [f32(max(-1.0, min(1.0, x))) for x in sig]


def signals():
    out = []
    rng = random.Random(1)
    # 1. single hits across the velocity range, well apart
    out.append(("single hits, soft to loud", place(SR * 3, [(int(SR * 0.25 * k), drum(rng, a)) for k, a in
                                                              enumerate([0.02, 0.05, 0.1, 0.2, 0.35, 0.5, 0.7, 0.9, 1.0, 0.3, 0.08])], rng)))
    # 2. 16ths at 120 BPM with accents and ghost notes
    ev = [(int(SR * 0.125 * k), drum(rng, 0.8 if k % 4 == 0 else 0.35 if k % 2 == 0 else 0.06)) for k in range(24)]
    out.append(("16ths at 120 BPM, accents and ghosts", place(SR * 3, ev, rng, 0.0005)))
    # 3. rolls getting faster (60 ms down to 18 ms)
    ev, t = [], 1000
    for k in range(40):
        gap = int(SR * (0.06 - k * 0.001))
        ev.append((t, drum(rng, rng.uniform(0.2, 0.6))))
        t += gap
    out.append(("roll speeding up 60 ms -> 20 ms", place(SR * 2, ev, rng, 0.0005)))
    # 4. flams: grace note 8..30 ms ahead
    ev = []
    for k, g in enumerate([8, 12, 16, 20, 25, 30]):
        at = int(SR * (0.1 + 0.4 * k))
        ev += [(at, drum(rng, 0.15)), (at + int(SR * g / 1000), drum(rng, 0.7))]
    out.append(("flams 8..30 ms", place(int(SR * 2.6), ev, rng)))
    # 5. ghost notes right around the threshold
    ev = [(int(SR * 0.1 * k) + 500, drum(rng, rng.uniform(0.005, 0.06))) for k in range(25)]
    out.append(("ghost notes near the threshold", place(int(SR * 2.6), ev, rng, 0.0003)))
    # 6. ringing tom: long sustain, then more hits into the ring
    ev = [(500, drum(rng, 0.9, body_hz=95, body_tau=0.6, n=SR * 2))]
    ev += [(int(SR * (0.4 + 0.35 * k)), drum(rng, rng.uniform(0.1, 0.5), body_hz=95, body_tau=0.4, n=SR)) for k in range(4)]
    out.append(("ringing tom with hits into the ring", place(int(SR * 2.5), ev, rng)))
    # 7. bleed: narrowband tone bursts (no noise) near the threshold, with real hits between
    ev = []
    for k in range(10):
        at = int(SR * 0.2 * k) + 300
        tone = [rng.uniform(0.03, 0.08) * sin(2 * pi * 180 * i / SR) * min(1, i / 240) * exp(-i / (SR * 0.05))
                for i in range(int(SR * 0.15))]
        ev.append((at, tone))
        if k % 3 == 0:
            ev.append((at + int(SR * 0.1), drum(rng, 0.5)))
    out.append(("narrowband bleed + real hits", place(int(SR * 2.2), ev, rng, 0.0002)))
    # 8. clipped loud hits and a fast attack (0 ms)
    ev = [(int(SR * 0.3 * k) + 100, drum(rng, rng.uniform(1.0, 3.0), attack_ms=0)) for k in range(6)]
    out.append(("clipped hits, instant attack", place(int(SR * 2), ev, rng)))
    # 9. borderline hits for the anti-bleed check: just over the threshold, slow attacks (the peak comes well after
    #    the crossing), from a pure tone to pure noise
    ev = []
    for k in range(48):
        r3 = random.Random(500 + k)
        amp, mix, att = r3.uniform(0.008, 0.02), r3.uniform(0.0, 1.0), r3.uniform(0.5, 4.0)
        hz, ph = r3.uniform(150, 2500), r3.uniform(0, 2 * pi)
        ev.append((int(SR * 0.09 * k) + 200, [amp * min(1.0, (i + 1) / (att * SR / 1000)) * exp(-i / (SR * 0.03)) *
                                             ((1 - mix) * sin(ph + 2 * pi * hz * i / SR) + mix * r3.uniform(-1, 1))
                                             for i in range(int(SR * 0.08))]))
    out.append(("borderline hits, tone to noise (anti-bleed)", place(int(SR * 4.5), ev, rng)))
    # 10. random everything
    for s in range(3):
        r2 = random.Random(100 + s)
        ev, t = [], r2.randint(0, 2000)
        while t < SR * 3:
            ev.append((t, drum(r2, r2.choice([r2.uniform(0.003, 0.04), r2.uniform(0.04, 1.2)]))))
            t += r2.randint(int(SR * 0.008), int(SR * 0.3))
        out.append((f"random hits, seed {s}", place(SR * 3, ev, r2, r2.choice([0, 0.0005, 0.003]))))
    return out


def param_sets():
    sets = [("patch defaults", dict(ref.DEFAULTS)),
            ("screenshot (Sens 1)", dict(ref.DEFAULTS, sens=1.0)),
            ("everything Fixed, CC on, ch 10, Fixed length 120 ms",
             dict(ref.DEFAULTS, vel_mode=1, vel_fixed=90, cc_on=1, cc_num=74, cc_mode=1, cc_fixed=64, channel=10,
                  len_mode=1, len_ms=120)),
            ("CC Dynamic + note Dynamic, short Fixed length 5 ms",
             dict(ref.DEFAULTS, cc_on=1, cc_num=1, len_mode=1, len_ms=5)),
            ("anti-bleed strong (Strict 0.5, Range 1), slow onset", dict(ref.DEFAULTS, spec_strict=0.5, spec_range=1.0,
                                                                        strict_db=2.0, speed_ms=1.0)),
            ("anti-bleed medium (Strict 0.3, Range 0.6)", dict(ref.DEFAULTS, spec_strict=0.3, spec_range=0.6))]
    rng = random.Random(7)
    for k in range(8):
        sets.append((f"random settings {k}", dict(
            ref.DEFAULTS, sens=rng.uniform(0.1, 10), thresh=rng.uniform(0, 31), retrig=rng.uniform(1, 16),
            mask_ms=rng.uniform(15, 64), scan_ms=rng.uniform(0, 4), curve=rng.uniform(0.1, 1.5),
            strict_db=rng.uniform(0, 20), speed_ms=rng.uniform(0.2, 10), spec_strict=rng.uniform(0, 0.6),
            spec_range=rng.uniform(0, 1), note=rng.randint(0, 127), vel_mode=rng.randint(0, 1),
            vel_fixed=rng.randint(1, 127), len_mode=rng.randint(0, 1), len_ms=rng.randint(5, 2000),
            cc_on=rng.randint(0, 1), cc_num=rng.randint(0, 127), cc_mode=rng.randint(0, 1),
            cc_fixed=rng.randint(1, 127), channel=rng.randint(1, 16))))
    return sets


def diff(a, b):
    for k in range(max(len(a), len(b))):
        x = a[k] if k < len(a) else None
        y = b[k] if k < len(b) else None
        if x != y:
            return f"first difference at event {k}: reference {x}, C {y}"
    return ""


# ---------------------------------------------------------------- run
sigs = signals()
psets = param_sets()
quick = "--quick" in sys.argv
total_ref_events = 0
for pname, d in psets:
    p, r = make_params(d)
    for sname, sig in (sigs[:4] + sigs[8:9] if quick else sigs):
        want = ref.run(sig, r)
        got = run_host(sig, p)
        total_ref_events += len(want)
        check(f"[{pname}] {sname}: {sum(1 for e in want if e[1] >> 4 == 9)} notes, {len(want)} MIDI messages, "
              "identical", want == got, diff(want, got))

# the M7 build: defaults and the Fixed / CC set on every signal
for pname, d in psets[:3] + psets[4:5]:
    p, r = make_params(d)
    for sname, sig in sigs:
        want = run_host(sig, p)
        got = run_arm(sig, p)
        check(f"[ARM {pname}] {sname}: same as the host build", want == got, diff(want, got))

# sanity on the detector itself (not just agreement): hits land and velocities follow the level
p, r = make_params(dict(ref.DEFAULTS))
ev = run_host(sigs[0][1], p)
notes = [e for e in ev if e[1] == 0x90]
check(f"single hits: {len(notes)} notes for 11 hits (soft hits into a loud one's ring are retrigger-cancelled)",
      len(notes) >= 8, notes)
vels = [e[3] for e in notes]
check(f"velocities rise with the level: {vels}", vels[:7] == sorted(vels[:7]), vels)
check("no stuck note: the last message is a note-off (a hit into a ringing note re-sends note-on, as in the patch)",
      ev[-1][1] == 0x80, ev[-3:])

# the anti-bleed check really decides something on the borderline signal (else the comparison above proves little)
bl = [x for x in sigs if x[0].startswith("borderline")][0][1]
on = sum(e[1] == 0x90 for e in run_host(bl, make_params(dict(ref.DEFAULTS, spec_strict=0.5, spec_range=1.0, strict_db=2.0, speed_ms=1.0))[0]))
off = sum(e[1] == 0x90 for e in run_host(bl, make_params(dict(ref.DEFAULTS, spec_strict=0.0, spec_range=0.0, strict_db=2.0, speed_ms=1.0))[0]))
check(f"anti-bleed rejects some borderline hits ({off} notes without it, {on} with it)", 0 < on < off, (on, off))

# CPU: instructions per sample on the M7 build
ips = count_instructions(sigs[1][1][:SR // 2], p)
cyc_budget = 400e6 / SR
print(f"M7 build: {ips:.0f} instructions per sample per input; two inputs ~ {2 * ips / cyc_budget * 100:.1f} % of a "
      f"400 MHz core at one instruction a cycle")
check("CPU: two inputs under 5 % of the core", 2 * ips / cyc_budget < 0.05, ips)

print(f"\n{total_ref_events} reference MIDI messages compared")
print("ALL PASS" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
