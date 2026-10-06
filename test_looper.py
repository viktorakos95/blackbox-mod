"""Looper step 1 (audio path test) under Unicorn.

Stubbed: the sample pool init, the permanent SDRAM allocator and its free-space query, the input stage's tail call
and the output packer (each records what it was handed).
NOT checked: that the tapped buffers really are the input and the outputs on the unit, levels at the jacks, and
which output pair is which (hardware).
"""
import struct
import sys

from emu import STACK, Emu

e = Emu("out/solo+slice+duck+chord+cond+filter+cpu+od+comp+seqfix+fx2+munchi+looper+cave/BLACKBOX.bin", "out/cave.elf")
e.uc.mem_map(0x38800000, 0x1000)                     # backup SRAM
e.uc.mem_map(0x58024000, 0x1000)                     # RCC / PWR
e.uc.mem_map(0xC0000000, 0x200000)                   # SDRAM
e.uc.mem_map(0x30000000, 0x40000)                    # fake buffers
ENGINE, BUF = 0x30000000, 0xC0100000
INL, INR, PTRS = 0x30010000, 0x30011000, 0x30012000
OUT = [0x30020000 + 0x1000 * i for i in range(6)]  # a, b, c of the left call, then of the right call
N = 256
LOOP = 2 * 48000
STATE = 0x38800C00

fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


free = {"bytes": 3 * 1024 * 1024}
log = []
e.stub(0x08070BF4, lambda: log.append(("pool", e.arg(0))))
e.stubs[0x080443F0] = lambda: e.ret(free["bytes"])
e.stub(0x0804436C, lambda: log.append(("alloc", e.arg(0), e.arg(1))), value=BUF)
e.stub(0x080518F0, lambda: log.append(("tail", e.arg(0), e.arg(1), e.arg(2))))
e.stub(0x0806002C, lambda: log.append(("pack", e.arg(0), e.arg(1), e.arg(2), e.arg(3), e.r32(STACK), e.r32(STACK + 4))))


def boot():
    log.clear()
    e.call("looper_boot", ENGINE)


def block(l, r):
    """One audio block: input stage, then the two packer calls. Returns the six output channels."""
    e.uc.mem_write(INL, struct.pack(f"<{N}f", *l))
    e.uc.mem_write(INR, struct.pack(f"<{N}f", *r))
    e.w32(PTRS, INL)
    e.w32(PTRS + 4, INR)
    log.clear()
    e.call("looper_in", 0x24005555, PTRS, N, count=5_000_000)
    tail = list(log)
    for o in OUT:
        e.uc.mem_write(o, struct.pack(f"<{N}f", *([0.25] * N)))
    e.w32(STACK, 0x24070000)
    e.w32(STACK + 4, 4)
    e.call("looper_out_l", OUT[0], OUT[1], OUT[2], N, count=5_000_000)
    e.w32(STACK, 0x24070008)
    e.w32(STACK + 4, 4)
    e.call("looper_out_r", OUT[3], OUT[4], OUT[5], N, count=5_000_000)
    outs = [struct.unpack(f"<{N}f", e.uc.mem_read(o, 4 * N)) for o in OUT]
    return tail, outs


def mode():
    return e.r32(STATE + 8)


boot()
check("boot: the stock pool init still runs, with the engine", ("pool", ENGINE) in log, log)
check("boot: 2 s of 16-bit stereo taken from the permanent allocator", ("alloc", LOOP * 4, 1) in log, log)
check("boot: armed, waiting for input", mode() == 1, mode())

tail, outs = block([0.001] * N, [-0.001] * N)
check("input stage tail call is made with the same arguments", tail == [("tail", 0x24005555, PTRS, N)], tail)
check("quiet input does not start a recording", mode() == 1, mode())
check("outputs untouched while armed", all(v == 0.25 for o in outs for v in o))

# a ramp so every frame is recognisable: L = +k, R = -k (scaled), starting mid-block at frame 100
def ramp(start, n, k0):
    l = [0.0] * n
    r = [0.0] * n
    for i in range(start, n):
        k = k0 + i - start
        l[i] = ((k % 1000) + 200) / 4000.0
        r[i] = -l[i]
    return l, r

l, r = ramp(100, N, 0)
block(l, r)
check("loud input starts recording", mode() == 2, mode())
recorded = N - 100
k = recorded
while recorded < LOOP:
    l, r = ramp(0, N, k)
    _, outs = block(l, r)
    recorded += N
    k += N
check("after 2 s of input: playing", mode() == 3, mode())
first = struct.unpack("<2h", e.uc.mem_read(BUF, 4))
check("first recorded frame is the trigger frame, stereo kept (L +, R -)", first[0] == int(200 / 4000 * 32767) and first[1] == -first[0], first)

# output: the loop is added on top of what the firmware rendered (0.25), lefts on the left call, rights on the right
pos = e.r32(STATE + 12)
_, outs = block([0.0] * N, [0.0] * N)
exp_l = [(((pos + i) % LOOP % 1000) + 200) / 4000.0 for i in range(N)]
ok_l = all(abs(outs[c][i] - 0.25 - exp_l[i]) < 1e-3 for c in range(3) for i in range(N))
ok_r = all(abs(outs[c][i] - 0.25 + exp_l[i]) < 1e-3 for c in range(3, 6) for i in range(N))
check("loop is added to all three left channels", ok_l, [round(outs[0][i] - 0.25, 4) for i in range(4)] + exp_l[:4])
check("and to all three right channels, same frames", ok_r)
check("packer still called with its stack arguments (dst, stride)", log[-1][5:] == (0x24070008, 4), log[-1])
check("play position advances once per block (on the right call)", e.r32(STATE + 12) == (pos + N) % LOOP, (pos, e.r32(STATE + 12)))
check("input while playing changes nothing", mode() == 3)

for _ in range(LOOP // N + 1):
    block([0.0] * N, [0.0] * N)
check("position wraps at the loop end", 0 <= e.r32(STATE + 12) < LOOP)

free["bytes"] = 1024 * 1024
boot()
check("boot with little SDRAM free: no allocation, looper stays off", not any(x[0] == "alloc" for x in log if isinstance(x, tuple)) and mode() == 0, (log, mode()))
tail, outs = block([0.5] * N, [0.5] * N)
check("...and the audio path is untouched", tail == [("tail", 0x24005555, PTRS, N)] and all(v == 0.25 for o in outs for v in o) and mode() == 0)

print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
