"""MCL MIDI jack (milestone M3) under Unicorn: src/mcl_bb/bb_midi.cpp, bb_m3test.cpp, the three hooks of patches/mclblob.py.

Real code runs for: the stock receive path (0x080446c4 -> our tap -> the stock ring push), the blob's TX hook and USART6 interrupt wrapper, the blob's
rings, the M3 self test state machine, and cpu.c / mcl_rt boot. Modelled in Python: the USART6 registers (TXE / TDR / CR1), the wire (loop cable,
Machinedrum with status and kit dump answers), time. Stubbed: the stock send function behind the hook (0x08044e04), the stock IRQ handler 0x080462e4.
NOT checked (hardware only): real USART6 timing and FIFO behaviour, the HAL's own CR1 writes, that the Machinedrum and the real kit dumps look like the model.
"""
import glob
import os
import shutil
import struct
import subprocess
import sys

from unicorn import UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0

from emu import Emu
from mcl_rig import BASE, MGR, NAMES, RAM, RAM_END, U, Rig, build, syms

BASE, RAM, RAM_END = 0x08040000, 0xC3F80000, 0xC4000000
U = 0x40011400
MGR = 0x24002E88
fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


PLAIN = build(False)
r = Rig(PLAIN)
e = r.e
image = open(r.img, "rb").read()
b = r.b

# --- the three hooks are in the image
from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs


def target(addr, size=4):
    ins = next(Cs(CS_ARCH_ARM, CS_MODE_THUMB).disasm(image[addr - BASE:addr - BASE + size], addr))
    return ins.mnemonic, int(ins.op_str.lstrip("#"), 16)


check("rx tap: the call at 0x080446d4 is now a bl to mcl_rx_tap", target(0x080446D4) == ("bl", b["mcl_rx_tap"]), target(0x080446D4))
vec = struct.unpack_from("<I", image, 0x0804015C - BASE)[0]
check("vector 71 points at mcl_usart6_isr (thumb)", vec == b["mcl_usart6_isr"] | 1, hex(vec))
check("send entry 0x08044dfc is a b.w to mcl_tx_hook", target(0x08044DFC) == ("b.w", b["mcl_tx_hook"]), target(0x08044DFC))

# --- before boot nothing is tapped or redirected, the stock code sees everything
r.rx(0x90)
check("before boot: stock ring got the byte, nothing went to MCL (no ready magic)", e.r32(MGR + 0x204) == 1 and e.uc.mem_read(MGR, 1)[0] == 0x90, e.r32(MGR + 0x204))
r.tx([0x90, 0x3C, 0x7F])
check("before boot: send goes to the stock function with the same arguments", r.stock_calls == [(0, 0x24070000, 3)], r.stock_calls)
r.isr(0x20)
check("before boot: the interrupt goes to the stock handler", r.irq_calls == 1)

# --- boot
lab = r.boot()
check("boot ok (' Mok'), hooks live", lab.endswith(" Mok") and r.call("bb_midi_tx_free") == 8192, lab)
r.tx([0x80, 0x40, 0x00])
check("not claimed yet: a send still goes to the stock function", r.stock_calls[-1] == (0, 0x24070000, 3))
n_before = len(r.stock_calls)
check("port 2 returns 0 without touching stock", r.call("mcl_tx_hook", 2, 0x24070000, 1) == 0 and len(r.stock_calls) == n_before)

# --- RX tap
n0 = e.r32(MGR + 0x204)
seq = [0xF0, 0x7D, 0x01, 0x7F, 0xF8, 0xF7, 0x90, 0x3C, 0x64]
for x in seq:
    r.rx(x)
r.rx(0xAA, port=1)
got = []
while (v := r.call("bb_midi_rx_get")) != 0xFFFFFFFF:
    got.append(v)
check("every jack byte reaches MCL's ring in order, port 1 does not", got == seq, got)
check("... and the stock ring got the same bytes (chained, unchanged)", e.r32(MGR + 0x204) == n0 + len(seq) and bytes(e.uc.mem_read(MGR + n0, len(seq))) == bytes(seq))
check("rx counters", r.call("bb_midi_stat", 0) == len(seq) and r.call("bb_midi_stat", 1) == 0)
for x in range(5000):
    r.rx(x & 0x7F)
check("rx ring full: bytes dropped and counted, never overwritten", r.call("bb_midi_stat", 1) == 5000 - 4096, r.call("bb_midi_stat", 1))
first = r.call("bb_midi_rx_get")
check("... oldest data kept", first == 0, first)
while r.call("bb_midi_rx_get") != 0xFFFFFFFF:
    pass

# --- claim, ownership
e.w32(0x24009914 + 0x80, 0x21)
r.call("bb_midi_claim", 1)
check("claim while the HAL is sending: refused", r.call("bb_midi_owner") == 0)
e.w32(0x24009914 + 0x80, 0x20)
r.call("bb_midi_claim", 0)
check("deferred claim waits for the next stock send", r.call("bb_midi_owner") == 0)
r.stock_calls.clear()
check("the next stock send takes ownership, is redirected (returns 1) and not sent by stock", r.tx([0x90, 0x3C, 0x7F]) == 1 and r.call("bb_midi_owner") == 1 and not r.stock_calls)
CR1 = 0x2D
e.w32(U, CR1 | 0x80)                                           # HAL bits + the TXEIE the redirect just set
r.sent.clear()
r.pump()
check("redirected message is sent byte-exact by the TXE interrupt; TXEIE is cleared when empty, other CR1 bits stay", r.sent == [0x90, 0x3C, 0x7F] and e.r32(U) == CR1, (r.sent, hex(e.r32(U))))

# --- order, real-time insertion, wrap, ring full
msg = [0x90 + (i & 15) for i in range(40)]
r.sent.clear()
buf = 0x24070100
e.uc.mem_write(buf, bytes(msg))
check("bb_midi_tx accepts", r.call("bb_midi_tx", buf, len(msg)) == len(msg))
for _ in range(3):
    r.isr()
check("three bytes out", r.sent == msg[:3])
check("real-time byte accepted", r.call("bb_midi_tx_rt", 0xF8) == 1)
r.isr()
check("the real-time byte goes out next, between the bytes of the message", r.sent[3] == 0xF8, r.sent)
r.pump()
check("the rest of the message follows, nothing lost or duplicated", [x for x in r.sent if x != 0xF8] == msg and r.sent.count(0xF8) == 1)
check("real-time slot fifo takes 16", all(r.call("bb_midi_tx_rt", 0xFE) == 1 for _ in range(16)))
check("seventeenth real-time byte while the first sixteen wait: refused and counted", r.call("bb_midi_tx_rt", 0xFE) == 0 and r.call("bb_midi_stat", 5) == 1)
r.pump()
r.sent.clear()
big = bytes((i * 7 + 3) & 0x7F for i in range(1000))
total = []
for rep in range(40):                                          # 40 KB through an 8 KB ring: wraps many times
    e.uc.mem_write(0x24071000, big)
    ok = r.call("bb_midi_tx", 0x24071000, len(big))
    assert ok == len(big)
    r.pump()
    total += list(big)
check("40,000 bytes through the wrapping ring arrive exactly and in order", r.sent == total, (len(r.sent), len(total)))
e.uc.mem_write(0x24071000, bytes(5000))
check("a message that does not fit is refused whole", r.call("bb_midi_tx", 0x24071000, 5000) == 5000 and r.call("bb_midi_tx", 0x24071000, 5000) == 0)
check("... counted, nothing partial queued (free space unchanged by the refusal)", r.call("bb_midi_stat", 3) == 1 and r.call("bb_midi_tx_free") == 8192 - 5000)
r.pump()
r.stock_calls.clear()
e.uc.mem_write(0x24071000, bytes(8000))
r.call("bb_midi_tx", 0x24071000, 8000)
check("stock send while the ring cannot take it returns 0 (the stock side retries)", r.tx([0x90, 1, 2, 3, 4, 5, 6, 7, 8, 9] * 30) == 0 and not r.stock_calls)
r.pump()

# --- the interrupt wrapper
r.irq_calls = 0
r.isr(0x80 | 0x40)
check("owner: a TXE-only interrupt is not passed on (the HAL must not see TXE)", r.irq_calls == 0)
r.isr(0x20 | 0x80)
check("owner: RXNE is passed to the stock handler", r.irq_calls == 1)
r.isr(0x08)
check("owner: an overrun flag is passed to the stock handler", r.irq_calls == 2)

# --- M3 hardware self test, with models
T = build(True)


def fresh(dev_factory):
    rg = Rig(T)
    rg.dev = dev_factory(rg)
    rg.call("mcl_boot")                                          # runtime + MIDI hooks; the blocking test is driven by `drive` below
    return rg


# The blocking test is driven here step by step with a model clock, because Unicorn has no cycle counter running: run boot (starts the blob), then loop.
def drive(rg, max_ms=4000):
    rg.call("bb_m3_reset")
    for ms in range(max_ms):
        done = rg.call("bb_m3_step", ms * 1000)
        rg.pump()
        if done:
            break
    res = bytes(rg.e.uc.mem_read(rg.call("bb_m3_result"), 20))
    return dict(zip(("loop", "md", "kit", "burst", "dump"), res[:5])), struct.unpack_from("<HI", res, 6), ms


def loop_cable(rg):
    return lambda byte: rg.rx(byte)


def machinedrum(kit=12, dumps=None, bad_second=False, truncate=False):
    state = {"buf": [], "n": 0}
    dump_body = bytes((i * 13 + 5) & 0x7F for i in range(1220))

    def dev(rg):
        def f(byte):
            if byte == 0xF0:
                state["buf"] = []
            state["buf"].append(byte)
            if byte == 0xF7:
                m = state["buf"]
                if m[:6] == [0xF0, 0, 0x20, 0x3C, 2, 0] and m[6] == 0x70 and m[7] == 2:
                    for x in [0xF0, 0, 0x20, 0x3C, 2, 0, 0x72, 2, kit, 0xF7]:
                        rg.rx(x)
                elif m[:6] == [0xF0, 0, 0x20, 0x3C, 2, 0] and m[6] == 0x53 and m[7] == kit:
                    body = bytearray(dump_body)
                    if bad_second and state["n"] == 1:
                        body[600] ^= 1
                    state["n"] += 1
                    out = [0xF0, 0, 0x20, 0x3C, 2, 0, 0x52, kit] + list(body) + ([] if truncate else [0xF7])
                    for x in out:
                        rg.rx(x)
        return f
    return dev


rg = fresh(lambda rg: None)
res, (dl, dc), ms = drive(rg)
check("M3 test, nothing connected: loop and MD both report no answer, the note burst ran", res["loop"] == 2 and res["md"] == 2 and res["dump"] == 0 and res["burst"] == 1, res)
check("... after 150 + 150 ms time-outs and the 1.7 s burst, not before", 1800 <= ms <= 1950, ms)
on = [(rg.sent[i], rg.sent[i + 1], rg.sent[i + 2]) for i in range(len(rg.sent) - 2) if rg.sent[i] in range(0x90, 0x94) and rg.sent[i + 2] == 110]
off = [(rg.sent[i], rg.sent[i + 1]) for i in range(len(rg.sent) - 2) if rg.sent[i] in range(0x80, 0x84) and rg.sent[i + 2] == 0]
check("... 16 note-ons on channels 1-4, notes 36/40/44/48, each followed by its note-off", len(on) == 16 and len(off) == 16 and on[0] == (0x90, 36, 110) and on[15] == (0x93, 48, 110) and [(a + 0xF0 & 0x0F | 0x80, b) for a, b, _ in on] == off, (len(on), len(off)))

rg = fresh(loop_cable)
res, _, ms = drive(rg)
check("M3 test with a cable from out to in: the 103-byte frame comes back byte-exact", res["loop"] == 1, res)
check("... and the burst (no Machinedrum answer) is heard on the wire too", rg.sent.count(0x90) >= 4, rg.sent.count(0x90))
check("... the loop-back also answers nothing for the Machinedrum", res["md"] == 2)

rg = fresh(machinedrum(kit=12))
res, (dl, dc), ms = drive(rg)
check("M3 test with a Machinedrum: status answer, kit 12, two identical dumps, no note burst", res["md"] == 1 and res["kit"] == 12 and res["dump"] == 1 and res["loop"] == 2 and res["burst"] == 0, res)
check("... dump frame length 1229 bytes and the same CRC both times", dl == 1229, dl)

rg = fresh(machinedrum(kit=63, bad_second=True))
res, _, _ = drive(rg)
check("M3 test: a dump that differs the second time is reported (dump = 2)", res["md"] == 1 and res["dump"] == 2, res)
rg = fresh(machinedrum(kit=5, truncate=True))
res, _, ms = drive(rg)
check("M3 test: a dump that never ends times out (dump = 3), no hang", res["md"] == 1 and res["dump"] == 3 and ms < 1500, (res, ms))

# --- the stock firmware is in the middle of a send when the test starts: reported as 'could not claim', not as 'no answer'
rg = fresh(machinedrum(kit=12))
rg.e.w32(0x24009914 + 0x80, 0x21)
res, _, _ = drive(rg)
check("M3 test: HAL busy at start -> loop = md = 3 (could not claim), nothing sent", res["loop"] == 3 and res["md"] == 3 and not rg.sent, (res, rg.sent))

# --- label text after a test run
rg = fresh(machinedrum(kit=12))
drive(rg)
rg.e.uc.mem_write(0x24061000, bytes(40))
rg.e.call(rg.b["mcl_status_text"], 0x24061000, 20)
txt = rg.e.cstr(0x24061000)
check("label text 'Mok L- D12 K+'", txt == "Mok L- D12 K+", txt)
rg.e.uc.mem_write(0x24061000, bytes(40))
end = rg.e.call(rg.b["mcl_status_text"], 0x24061000, 5)
check("room limit is respected: 5 characters written, no more", rg.e.cstr(0x24061000) == "Mok L", rg.e.cstr(0x24061000))

# --- the blocking driver itself (mcl_gui): runs the same state machine against the cycle counter; nothing connected here, and no interrupts
#     are modelled, so it must end by its own time-outs after about 300 ms of the 400 MHz clock (the TX ring just fills)
from unicorn import UC_HOOK_MEM_READ
rg = Rig(T)
dwt = {"c": 0}


def dwt_read(uc, t, a, sz, v, _):
    dwt["c"] += 4000                                            # every read of the cycle counter: 10 us
    uc.mem_write(0xE0001004, struct.pack("<I", dwt["c"] & 0xFFFFFFFF))


rg.e.uc.hook_add(UC_HOOK_MEM_READ, dwt_read, begin=0xE0001004, end=0xE0001007)
rg.call("mcl_boot")
rg.e.call(rg.b["mcl_gui"], count=20_000_000)
res = bytes(rg.e.uc.mem_read(rg.call("bb_m3_result"), 4))
check("mcl_gui: blocking test ends by itself, loop and MD report no answer", res[0] == 2 and res[1] == 2, res.hex())
check("... after about 1.9 s (300 ms time-outs + the note burst, ~750 M cycles)", 700_000_000 < dwt["c"] < 800_000_000, dwt["c"])
rg.e.uc.mem_write(0x24061000, bytes(40))
rg.e.call(rg.b["mcl_status_text"], 0x24061000, 26)
check("label text after a test with nothing connected: 'Mok L- D- N'", rg.e.cstr(0x24061000) == "Mok L- D- N", rg.e.cstr(0x24061000))
before = dwt["c"]
rg.e.call(rg.b["mcl_gui"], count=20_000_000)
check("mcl_gui runs only once per power-up", dwt["c"] == before)

build(False)                                                   # leave out/ in the normal state for the other tests
print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
