"""callgraph.py: direct-call graph of the stock image (bl / b.w targets, function = the nearest earlier push {..lr} or a bl target) and which of the FatFs entry points are reachable from each FreeRTOS task root.
Virtual calls (blx through a vtable) are NOT followed: 'unreached' means 'not by direct calls'."""
import sys, os, struct, collections, bisect
from capstone import *
BASE = 0x08040000
fw = open(os.environ.get("FW", "firmware/BLACKBOX-3.1.9.bin"), "rb").read()
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB); md.skipdata = True
ins = list(md.disasm(fw[:0xB2000], BASE))
bl = [(i.address, int(i.op_str[1:], 16)) for i in ins if i.mnemonic in ("bl", "b.w") and i.op_str.startswith("#")]
targets = sorted({t for _, t in bl if BASE <= t < BASE + 0xB2000} | {a for a in range(BASE, BASE + 0xB2000, 2) if False})
pushes = [i.address for i in ins if i.mnemonic == "push" and "lr" in i.op_str or i.mnemonic == "push.w" and "lr" in i.op_str]
starts = sorted(set(targets) | set(pushes))
def func_of(a):
    k = bisect.bisect_right(starts, a) - 1
    return starts[k] if k >= 0 else None
calls = collections.defaultdict(set)
for a, t in bl:
    f = func_of(a)
    if f is not None and t in set(targets): calls[f].add(t)
FAT = {0x8086534: "f_open", 0x8086866: "f_read", 0x8086a06: "f_write", 0x8086d76: "f_close", 0x8086d96: "f_lseek", 0x8086c28: "f_sync", 0x80864e8: "f_mount"}
roots = {"defaultTask": 0x8043dec, "audioTask": 0x8041470, "pcmStreamer": 0x804c30c}
def reach(r):
    seen, st = {r}, [r]
    while st:
        f = st.pop()
        for t in calls.get(f, ()):
            if t not in seen: seen.add(t); st.append(t)
    return seen
R = {n: reach(a) for n, a in roots.items()}
for n, s in R.items():
    print(n, "reaches", len(s), "functions;", "FatFs:", sorted({FAT[a] for a in s if a in FAT}))
# who uses FatFs, and from where
users = collections.defaultdict(set)
for a, t in bl:
    if t in FAT: users[func_of(a)].add(FAT[t])
print("\nfunctions calling FatFs:", len(users))
for f in sorted(users):
    tasks = [n for n, s in R.items() if f in s]
    print(hex(f), sorted(users[f]), tasks)
