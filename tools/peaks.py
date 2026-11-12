"""peaks.py [samplr] [blocks]: where the uneven work is. Runs the scenario of tools/prof_samplr.py (three SAMPLR tracks: arp, grain cloud,
slicer loop) for a number of 256-frame blocks under Unicorn, counts the instructions of every block by function, and prints the
average block, the heaviest blocks, and for those the functions that took more than they do on average (the bursts that make a peak).
Instructions, not cycles: compare blocks with each other, do not read it as the unit's load."""
import bisect
import os
sys_path_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
import collections
import subprocess
import sys
sys.path.insert(0, sys_path_root)

NB = int(sys.argv[2]) if len(sys.argv) > 2 else 200
src = open("tools/prof_samplr.py").read().split("syms = sorted(")[0]
sys.argv = [sys.argv[0]]
exec(src)

from unicorn import UC_HOOK_CODE

syms = sorted((int(l.split()[0], 16), l.split()[2]) for l in subprocess.run(["arm-none-eabi-nm", "out/cave.elf"], capture_output=True, text=True).stdout.splitlines()
              if len(l.split()) == 3 and l.split()[1] in "tTwW")
addrs = [a for a, _ in syms]
cur = collections.Counter()


LINES = os.environ.get("PEAK_LINES")                               # (with a -g build: by source line too)
curpc = collections.Counter()


def hk(uc, a, size, _):
    i = bisect.bisect_right(addrs, a) - 1
    cur[syms[i][1] if i >= 0 else "?"] += 1
    if LINES:
        curpc[a] += 1


h = e.uc.hook_add(UC_HOOK_CODE, hk)
blocks, pcb = [], []
for _ in range(NB):
    cur.clear()
    curpc.clear()
    play()
    blocks.append(collections.Counter(cur))
    if LINES:
        pcb.append(collections.Counter(curpc))
e.uc.hook_del(h)
tot = [sum(b.values()) for b in blocks]
mean = sum(tot) / len(tot)
avg = collections.Counter()
for b in blocks:
    avg.update(b)
srt = sorted(tot)
print(f"{NB} blocks: mean {mean:.0f}  median {srt[len(srt) // 2]}  p95 {srt[int(len(srt) * .95)]}  max {srt[-1]}  (max / mean {srt[-1] / mean:.2f})")
for i in sorted(range(NB), key=lambda k: -tot[k])[:5]:
    b = blocks[i]
    extra = sorted(((b[f] - avg[f] / NB, f) for f in set(b) | set(avg)), reverse=True)[:6]
    print(f"block {i}: {tot[i]} (+{tot[i] - mean:.0f})  " + "  ".join(f"{f} +{x:.0f}" for x, f in extra if x > 500))

if LINES:
    allpc = collections.Counter()
    for b in pcb:
        allpc.update(b)
    ad = sorted(allpc)
    out = subprocess.run(["arm-none-eabi-addr2line", "-e", "out/cave.elf"] + ["%x" % a for a in ad], capture_output=True, text=True).stdout.splitlines()
    line = {a: l.split("/")[-1].split(" ")[0] for a, l in zip(ad, out)}
    worst = max(range(NB), key=lambda k: tot[k])
    wl, al = collections.Counter(), collections.Counter()
    for a, v in pcb[worst].items():
        wl[line[a]] += v
    for a, v in allpc.items():
        al[line[a]] += v / NB
    print("heaviest block, lines above their average:")
    for x, l in sorted(((wl[l] - al[l], l) for l in set(wl) | set(al)), reverse=True)[:25]:
        print(f"   {l:24s} +{x:9.0f}")
