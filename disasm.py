"""disasm.py <start> <end>  — Thumb-2 disassembly of the stock image (capstone)."""
import sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB

BASE = 0x08040000
fw = open(__file__.rsplit("/", 1)[0] + "/firmware/BLACKBOX-3.1.9.bin", "rb").read()
s, e = (int(x, 16) for x in sys.argv[1:3])
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
for i in md.disasm(fw[s - BASE:e - BASE], s):
    print(f"{i.address:08x}  {i.bytes.hex():10} {i.mnemonic:8} {i.op_str}")
