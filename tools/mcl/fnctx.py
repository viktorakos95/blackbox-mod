"""fnctx.py <addr hex> ... : for each address, the start of the enclosing function (nearest earlier push {..lr} in a 2-aligned scan) and the strings its code loads (pc-relative literals that point into rodata)."""
import sys, struct, os
from capstone import *
BASE = 0x08040000
fw = open(os.environ.get("FW", "firmware/BLACKBOX-3.1.9.bin"), "rb").read()
def w(a): return struct.unpack("<I", fw[a-BASE:a-BASE+4])[0]
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB); md.skipdata = True
def func_start(a):
    for p in range(a & ~1, a - 0x600, -2):
        h = int.from_bytes(fw[p-BASE:p-BASE+2], "little")
        if h & 0xFE00 == 0xB400 and h & 0x100:           # push {..., lr}
            return p
        if h == 0xE92D and int.from_bytes(fw[p-BASE+2:p-BASE+4], "little") & 0x4000:   # push.w {..., lr}
            return p
    return None
def strings_in(f, n=0x400):
    out = []
    for i in md.disasm(fw[f-BASE:f-BASE+n], f):
        if i.mnemonic.startswith("ldr") and "[pc" in i.op_str and "#" in i.op_str:
            a = ((i.address + 4) & ~3) + int(i.op_str.split("#")[1].rstrip("]"), 0)
            if BASE <= a < BASE + len(fw) - 4:
                v = w(a)
                if BASE <= v < BASE + len(fw) - 40:
                    s = fw[v-BASE:v-BASE+60].split(b"\0")[0]
                    if len(s) >= 4 and all(32 <= c < 127 for c in s): out.append(s.decode())
    return out
if __name__ == "__main__":
    for x in sys.argv[1:]:
        a = int(x, 16); f = func_start(a)
        print(hex(a), "in", hex(f) if f else None, strings_in(f, 0x300)[:6] if f else "")
