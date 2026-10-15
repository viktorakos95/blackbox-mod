"""DRONE-8 (lira_page_run) cost per 256-frame block under Unicorn: instructions by function, one key held (or all eight
with the argument 8). Setup as test_lira_gov.py; the load seen by the governor is kept low so nothing sheds."""
import bisect
import collections
import subprocess
import sys

from unicorn import UC_HOOK_CODE

from emu import Emu

IMG = "out/solo+slice+duck+chord+cond+filter+cpu+od+comp+seqfix+fx2+munchi+lira+cave/BLACKBOX.bin"
e = Emu(IMG, "out/cave.elf")
e.uc.mem_map(0xC0000000, 0x400000)
e.uc.mem_map(0x38800000, 0x1000)
e.uc.mem_map(0x58024000, 0x1000)
e.uc.mem_map(0xE0000000, 0x10000)
e.stub(0x0808C0D8)
e.stub(0x0808C12C)
e.stub(0x0804436C, value=0xC0000000)
e.w32(0x24000088, 0xC4000000)
e.w32(0x2400008C, 0xC0000000)
e.call("interp_register", 0x24060000, 0xF1, 0, 0, 2, 0)
N, PER = 256, 2_560_000
L, R = 0x24070000, 0x24071000
cyc = [10_000_000]


def block():
    cyc[0] += PER
    e.w32(0xE0001004, cyc[0] & 0xFFFFFFFF)
    e.w32(0x2405FFD4, (cyc[0] - int(PER * 0.2)) & 0xFFFFFFFF)
    e.uc.mem_write(L, bytes(4 * N))
    e.uc.mem_write(R, bytes(4 * N))
    e.call("lira_page_run", L, R, N, count=50_000_000)


keys = int(sys.argv[1]) if len(sys.argv) > 1 else 1
for k in range(keys):
    e.call("lira_ui_key", k, 1)
for _ in range(80):
    block()
syms = sorted((int(l.split()[0], 16), l.split()[2]) for l in subprocess.run(["arm-none-eabi-nm", "out/cave.elf"], capture_output=True, text=True).stdout.splitlines() if len(l.split()) == 3 and l.split()[1] in "tTwW")
addrs = [a for a, _ in syms]
cnt = collections.Counter()
tot = [0]


def hk(uc, a, size, _):
    tot[0] += 1
    i = bisect.bisect_right(addrs, a) - 1
    cnt[syms[i][1] if i >= 0 else "?"] += 1


h = e.uc.hook_add(UC_HOOK_CODE, hk)
NB = 4
for _ in range(NB):
    block()
e.uc.hook_del(h)
print("keys", keys, "instructions/block:", tot[0] // NB)
for k, v in cnt.most_common(15):
    print(f"   {k:28s} {v // NB:9d} {100 * v / tot[0]:5.1f}%")
