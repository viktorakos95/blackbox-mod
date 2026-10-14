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

NAMES = "solo slice duck chord cond filter cpu od comp seqfix fx2 munchi looper clkmon cave mclblob".split()
BASE, RAM, RAM_END = 0x08040000, 0xC3F80000, 0xC4000000
U = 0x40011400
MGR = 0x24002E88
fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


def build(test):
    env = dict(os.environ, M3TEST="1" if test else "")
    subprocess.run(["tools/mcl/build_blob.sh"], check=True, env=env, capture_output=True)
    subprocess.run(["python3", "patch.py"] + NAMES, check=True, capture_output=True)
    img = glob.glob("out/*+cave+mclblob/BLACKBOX.bin")[0]
    d = "out/m3test" if test else "out/m3plain"
    os.makedirs(d, exist_ok=True)
    shutil.copy(img, d + "/BLACKBOX.bin")
    shutil.copy("out/mcl.elf", d + "/mcl.elf")
    return d


def syms(path):
    return {l.split()[2]: int(l.split()[0], 16) for l in subprocess.run(["arm-none-eabi-nm", path], capture_output=True, text=True, check=True).stdout.splitlines() if len(l.split()) == 3}


def emulate_primask(rg):
    """Unicorn's ARM model has no M-profile special registers (mrs / msr / cpsid / cpsie are invalid instructions there). The test copy of the
    blob in emulator memory gets them replaced by `movs rN,#0` / nop: single-threaded here, so there is nothing to mask. The image on disk is untouched.
    Instruction addresses come from the blob's own disassembly (objdump), which knows where the literal pools are."""
    dump = subprocess.run(["arm-none-eabi-objdump", "-d", "--no-show-raw-insn", rg.elf], capture_output=True, text=True, check=True).stdout
    for line in dump.splitlines():
        f = line.split("\t")
        if len(f) < 2 or not f[0].strip().endswith(":"):
            continue
        addr, mn = int(f[0].strip()[:-1], 16), f[1].strip()
        if mn == "mrs":
            rn = int(f[2].split(",")[0].strip()[1:])
            rep = struct.pack("<HH", 0x2000 | (rn << 8), 0xBF00)              # movs rN, #0 ; nop
        elif mn == "msr":
            rep = b"\x00\xbf\x00\xbf"
        elif mn in ("cpsid", "cpsie"):
            rep = b"\x00\xbf"
        else:
            continue
        rg.e.uc.mem_write(addr, rep)


class Rig:
    """The unit: image + modelled USART6 + wire."""

    def __init__(self, d):
        self.img, self.elf, self.b = d + "/BLACKBOX.bin", d + "/mcl.elf", syms(d + "/mcl.elf")
        e = self.e = Emu(self.img, "out/cave.elf")
        u = e.uc
        u.mem_map(0xE0000000, 0x100000)
        u.mem_map(0x38800000, 0x1000)
        u.mem_map(0x58024000, 0x1000)
        u.mem_map(0x40011000, 0x1000)
        u.mem_map(RAM, RAM_END - RAM)
        u.mem_write(RAM, b"\xa5" * (RAM_END - RAM))
        u.mem_write(0x38800000, b"\x5a" * 0x1000)
        e.w32(0x24000088, 0xC3F00000)
        e.w32(0x2400008C, 0xC0000000 + 40 * 1024 * 1024)
        e.w32(0x24009914 + 0x80, 0x20)                         # HAL gState READY
        e.w32(MGR + 0x20D - 3, 0x01000000)                     # stock ring 0 active flag byte at mgr+0x20d
        e.stub(0x080A3DEC)
        e.uc.mem_write(0x24060000, bytes(0x60))
        e.uc.mem_write(0x24060000 + 0x31, e.cstr(0x080CF290).encode() + b"\0")
        e.uc.mem_write(0x2405FFD0, b"\xa5" * 36)
        self.sent, self.stock_calls, self.irq_calls = [], [], 0
        e.stub(0x08044E04, lambda: self.stock_calls.append((e.arg(0), e.arg(1), e.arg(2))), value=1)
        e.stub(0x080462E4, self._stock_irq)
        u.hook_add(UC_HOOK_MEM_WRITE, lambda uc, t, a, sz, v, _: self.sent.append(v & 0xFF), begin=U + 0x28, end=U + 0x2B)
        self.dev = None
        self.clock = 0
        emulate_primask(self)

    def _stock_irq(self):
        self.irq_calls += 1

    def call(self, name, *a):
        self.e.call(self.b[name], *a)
        return self.e.uc.reg_read(UC_ARM_REG_R0)

    def boot(self):
        self.e.call("cpu_text_b", 0x24060000, 0x080CF290)
        return self.e.cstr(0x24060000 + 0x31)

    def isr(self, isr_flags=0x80 | 0x40):
        self.e.w32(U + 0x1C, isr_flags)
        self.e.call(self.b["mcl_usart6_isr"])

    def rx(self, byte, port=0):
        self.e.call(0x080446C4, MGR, byte, port)

    def pump(self):
        """Hardware transmit side: while CR1.TXEIE is set the TXE interrupt fires; each byte goes out on the wire (and into the device model)."""
        n = 0
        while self.e.r32(U) & 0x80 and n < 100000:
            before = len(self.sent)
            self.isr()
            for b in self.sent[before:]:
                if self.dev:
                    self.dev(b)
            n += 1
        return n

    def tx(self, data):
        buf = 0x24070000
        self.e.uc.mem_write(buf, bytes(data))
        self.e.call(self.b["mcl_tx_hook"], 0, buf, len(data))
        return self.e.uc.reg_read(UC_ARM_REG_R0)


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
check("real-time slot fifo takes 4", all(r.call("bb_midi_tx_rt", 0xFE) == 1 for _ in range(4)))
check("fifth real-time byte while the first four wait: refused", r.call("bb_midi_tx_rt", 0xFE) == 0)
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
    return dict(zip(("loop", "md", "kit", "dump"), res[:4])), struct.unpack_from("<HxxI", res, 4), ms


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
check("M3 test, nothing connected: loop and MD both report no answer", res["loop"] == 2 and res["md"] == 2 and res["dump"] == 0, res)
check("... after the 150 ms + 150 ms time-outs, not before", 290 <= ms <= 330, ms)

rg = fresh(loop_cable)
res, _, ms = drive(rg)
check("M3 test with a cable from out to in: the 103-byte frame comes back byte-exact", res["loop"] == 1, res)
check("... the loop-back also answers nothing for the Machinedrum", res["md"] == 2)

rg = fresh(machinedrum(kit=12))
res, (dl, dc), ms = drive(rg)
check("M3 test with a Machinedrum: status answer, kit 12, two identical dumps", res["md"] == 1 and res["kit"] == 12 and res["dump"] == 1 and res["loop"] == 2, res)
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
check("... after about 300 ms (~120 M cycles)", 100_000_000 < dwt["c"] < 140_000_000, dwt["c"])
rg.e.uc.mem_write(0x24061000, bytes(40))
rg.e.call(rg.b["mcl_status_text"], 0x24061000, 26)
check("label text after a test with nothing connected: 'Mok L- D-'", rg.e.cstr(0x24061000) == "Mok L- D-", rg.e.cstr(0x24061000))
before = dwt["c"]
rg.e.call(rg.b["mcl_gui"], count=20_000_000)
check("mcl_gui runs only once per power-up", dwt["c"] == before)

build(False)                                                   # leave out/ in the normal state for the other tests
print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
