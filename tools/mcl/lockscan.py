"""lockscan.py <target hex> : for every `bl target`, print the call site and the constant loaded into r0/r1 just before it (PC-relative literal), to find which objects (mutexes) a lock helper is used on."""
import sys, struct, os
from capstone import *
BASE = 0x08040000
fw = open(os.environ.get("FW", "firmware/BLACKBOX-3.1.9.bin"), "rb").read()
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB); md.detail = False; md.skipdata = True
tgt = int(sys.argv[1], 16)
ins = list(md.disasm(fw[0:0xB2000], BASE))
idx = {i.address: n for n, i in enumerate(ins)}
def lit(i):
    # ldr rX, [pc, #imm] -> value
    if i.mnemonic.startswith("ldr") and "[pc" in i.op_str:
        imm = int(i.op_str.split("#")[1].rstrip("]"), 0) if "#" in i.op_str else 0
        a = ((i.address + 4) & ~3) + imm
        return i.op_str.split(",")[0], struct.unpack("<I", fw[a - BASE:a - BASE + 4])[0]
for n, i in enumerate(ins):
    if i.mnemonic == "bl" and i.op_str == "#" + hex(tgt):
        regs = {}
        for j in ins[max(0, n - 8):n]:
            l = lit(j)
            if l: regs[l[0]] = l[1]
        print(hex(i.address), {k: hex(v) for k, v in regs.items()})
