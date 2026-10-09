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
    cfg = [(3, 1 << 3), (2, 1 << 2), (0, 1 << 0), (1, 1 << 1)]
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
if len(sys.argv) > 1:
    L_OFF = struct_offsets(['load'])['load']
    for tt in range(4):
        e.call('samplr_track', tt, count=50_000_000); e.call('samplr'); SMP = e.uc.reg_read(A.UC_ARM_REG_R0)
        e.uc.mem_write(SMP + L_OFF, struct.pack('<H', 600))
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
