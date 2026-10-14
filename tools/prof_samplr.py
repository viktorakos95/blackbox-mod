exec(open("tools/prof_head.py").read())
import bisect, collections
from unicorn import UC_HOOK_CODE
syms = sorted((int(l.split()[0], 16), l.split()[2]) for l in subprocess.run(["arm-none-eabi-nm", "out/cave.elf"], capture_output=True, text=True).stdout.splitlines() if l.split()[1] in "tTwW" )
addrs = [a for a, _ in syms]
cnt = collections.Counter()
tot = [0]
def hk(uc, a, size, _):
    tot[0] += 1
    i = bisect.bisect_right(addrs, a) - 1
    cnt[syms[i][1] if i >= 0 else "?"] += 1
def setb(off, v):
    e.uc.mem_write(SMP + off, bytes([v]))
def scenario():
    # track 0: grain, 4 fingers, latched; track 1: arp, 4 spots latched; track 2: slicer loops latched; track 3: tape holds latched
    cfg = [(3, 1 << 3), (3, 1 << 3), (2, 1 << 2), (0, 1 << 0)]   # two grain tracks, an arp, slicer loops
    for t, (mode, lat) in enumerate(cfg):
        e.call("samplr_track", t, count=50_000_000)
        e.call("samplr")
        global SMP
        SMP = e.uc.reg_read(A.UC_ARM_REG_R0)
        e.call("samplr_set_mode", mode)
        setb(OFF["latchm"], lat)
        if mode == 0:
            setb(OFF["loopm"], 1)
        setb(OFF["div"], 3)
        e.call("samplr_trans", 3)
        for f in range(4):
            e.call("samplr_touch", 0, f, 100 + 200 * f, 300)
            e.call("samplr_touch", 2, f, 0, 0)
scenario()
import sys
SHED = int(sys.argv[1]) if len(sys.argv) > 1 else 0
def scr_off(name):
    src = '#include "src/samplr.h"\nchar o_x[__builtin_offsetof(struct smscr,%s)];\n' % name
    out = subprocess.run(["arm-none-eabi-gcc", "-I.", "-mthumb", "-S", "-x", "c", "-", "-o", "-"], input=src, capture_output=True, text=True, check=True).stdout.splitlines()
    return int([l.split()[1] for l in out if l.strip().startswith(".space")][0])
e.call("samplr_track", 0, count=50_000_000); e.call("samplr"); SMP = e.uc.reg_read(A.UC_ARM_REG_R0)
SC = struct.unpack("<I", e.uc.mem_read(SMP + struct_offsets(["sc"])["sc"], 4))[0]
e.uc.mem_write(SC + scr_off("shed"), bytes([SHED]))
e.uc.mem_write(SC + scr_off("shed_t"), struct.pack("<I", 1 << 30))
for _ in range(30):
    block([0.0] * N)
h = e.uc.hook_add(UC_HOOK_CODE, hk)
NB = 8
tot[0] = 0
for _ in range(NB):
    block([0.0] * N)
e.uc.hook_del(h)
print("instructions per block:", tot[0] // NB)
for k, v in cnt.most_common(18):
    print(f"{k:28s} {v // NB:9d} {100 * v / tot[0]:5.1f}%")
