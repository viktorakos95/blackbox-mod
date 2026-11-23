"""SAMPLR's cost per 256-frame block under Unicorn: instructions by function (and, with the argument `lines`, by source line; build the
cave with -g for that). Scenario: three tracks at once, as played on the unit: track 1 an arpeggio (latched, three spots), track 2 a grain
cloud (latched, three fingers' clouds), track 0 the slicer in LOOP mode with a finger down. Setup as test_samplr.py."""
import bisect
import collections
import subprocess
import sys

src = open("test_samplr.py").read().split("# SLICER, slice 3 of 16")[0]
exec(src)

from unicorn import UC_HOOK_CODE

SM_SLICER, SM_TAPE, SM_ARP, SM_GRAIN = 0, 1, 2, 3
lines_mode = len(sys.argv) > 1 and sys.argv[1] == "lines"


def track_state(t):
    e.call("samplr_track", t, count=50_000_000)
    e.call("samplr")
    return e.uc.reg_read(A.UC_ARM_REG_R0)


# track 1: an arp, latched (mode 2, latch bit 4), three spots
SMP0 = SMP
SMP = track_state(1)
e.call("samplr_set_mode", SM_ARP)
e.uc.mem_write(SMP + OFF["latchm"], b"\x04")
for i, x in enumerate((200, 500, 800)):
    touch(0, i, x, 300 + 150 * i)
    touch(2, i, 0, 0)
play(20)
# track 2: a grain cloud, latched (mode 3, latch bit 8), three fingers
SMP = track_state(2)
e.call("samplr_set_mode", SM_GRAIN)
e.uc.mem_write(SMP + OFF["latchm"], b"\x08")
for i, x in enumerate((250, 550, 850)):
    touch(0, i, x, 400)
    touch(2, i, 0, 0)
play(20)
# track 0 shown: the slicer in LOOP mode with a finger down
SMP = track_state(0)
e.call("samplr_set_mode", SM_SLICER)
e.uc.mem_write(SMP + OFF["loopm"], b"\x01")
touch(0, 0, 5 * 64 + 10, 100)
touch(0, 1, 9 * 64 + 10, 100)
play(60)

syms = sorted((int(l.split()[0], 16), l.split()[2]) for l in subprocess.run(["arm-none-eabi-nm", "out/cave.elf"], capture_output=True, text=True).stdout.splitlines() if len(l.split()) == 3 and l.split()[1] in "tTwW")
addrs = [a for a, _ in syms]
cnt = collections.Counter()
pcs = collections.Counter()
tot = [0]


def hk(uc, a, size, _):
    tot[0] += 1
    if lines_mode:
        pcs[a] += 1
    i = bisect.bisect_right(addrs, a) - 1
    cnt[syms[i][1] if i >= 0 else "?"] += 1


if len(sys.argv) > 2 and sys.argv[1] == "ref":                 # record the output of 120 blocks (to prove a change keeps the sound): ref FILE
    import pickle
    rec = []
    for _ in range(120):
        o = play()
        rec.append((list(o[0]), list(o[1])))
    pickle.dump(rec, open(sys.argv[2], "wb"))
    print("recorded", len(rec), "blocks")
    sys.exit(0)
h = e.uc.hook_add(UC_HOOK_CODE, hk)
NB = 6
for _ in range(NB):
    play()
e.uc.hook_del(h)
lv = [sm(OFF["load"], "H") if False else 0]
print("instructions per block (whole audio call):", tot[0] // NB)
for k, v in cnt.most_common(18):
    print(f"   {k:28s} {v // NB:9d} {100 * v / tot[0]:5.1f}%")
if lines_mode:
    ad = sorted(pcs)
    out = subprocess.run(["arm-none-eabi-addr2line", "-e", "out/cave.elf"] + ["%x" % a for a in ad], capture_output=True, text=True).stdout.splitlines()
    ln = collections.Counter()
    for a, l in zip(ad, out):
        ln[l.split("/")[-1].split(" ")[0]] += pcs[a]
    print("by line:")
    for l, v in ln.most_common(45):
        print(f"   {l:22s} {v // NB:8d} {100 * v / tot[0]:5.1f}%")
