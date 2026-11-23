import sys,collections
from capstone import *
BASE=0x08040000
fw=open(__import__("os").environ.get("FW","firmware/BLACKBOX-3.1.9.bin"),"rb").read()
md=Cs(CS_ARCH_ARM,CS_MODE_THUMB); md.skipdata=True
lo,hi=int(sys.argv[1],16),int(sys.argv[2],16)
callers=collections.defaultdict(list)
for i in md.disasm(fw[0:0xB2000],BASE):
    if i.mnemonic in("bl","b.w") and i.op_str.startswith("#"):
        t=int(i.op_str[1:],16)
        if lo<=t<hi and i.mnemonic=="bl": callers[t].append(i.address)
for t in sorted(callers): print(hex(t),len(callers[t]),[hex(x) for x in callers[t][:4]])
