"""SD recon image (milestone M7): points every stock call of the FatFs entry points, and the SD card driver's read / write / ioctl table slots, at the counting
wrappers in src/mcl_bb/bb_sdspy.cpp. Apply AFTER `mclblob` (the wrappers are in the blob):  python3 patch.py ... cave mclblob mclspy
Build the blob with SDSPY=1 (tools/mcl/build_blob.sh) so the MCL page shows the numbers. Test image only."""
import os
import struct

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs

from thumb import ROOT, b_w, bl, symbols

BASE = 0x08040000
bsym = symbols(os.path.join(ROOT, "out", "mcl.elf"))
stock = open(os.path.join(ROOT, "firmware", "BLACKBOX-3.1.9.bin"), "rb").read()

FAT = {0x08086534: "spy_f_open", 0x08086866: "spy_f_read", 0x08086a06: "spy_f_write", 0x08086d76: "spy_f_close", 0x08086d96: "spy_f_lseek",
       0x08086c28: "spy_f_sync", 0x080864e8: "spy_f_mount"}
EXPECT = {"spy_f_open": 11, "spy_f_read": 30, "spy_f_write": 20, "spy_f_close": 13, "spy_f_lseek": 11}          # call counts found in M0 (docs/mcl-m0-m1-results.md)

md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
md.skipdata = True
PATCHES = []
found = {}
for i in md.disasm(stock[:0xB2000], BASE):
    if i.mnemonic in ("bl", "b.w") and i.op_str.startswith("#"):
        t = int(i.op_str[1:], 16)
        if t in FAT:
            name = FAT[t]
            old = bytes(i.bytes)
            new = bl(i.address, bsym[name]) if i.mnemonic == "bl" else b_w(i.address, bsym[name])
            assert len(old) == 4 and len(new) == 4
            PATCHES.append((i.address, old, new))
            found.setdefault(name, []).append((i.address, i.mnemonic))
for name, n in EXPECT.items():
    got = len([1 for _, m in found.get(name, []) if m == "bl"])
    assert got == n, f"{name}: found {got} bl call sites, M0 counted {n}"

# the card driver table (Diskio_drvTypeDef at 0x080cfc1c: init, status, read, write, ioctl)
for slot, name, orig in ((0x080CFC24, "spy_disk_read", 0x08045975), (0x080CFC28, "spy_disk_write", 0x080459E9), (0x080CFC2C, "spy_disk_ioctl", 0x08045A5D)):
    assert struct.unpack("<I", stock[slot - BASE:slot - BASE + 4])[0] == orig
    PATCHES.append((slot, struct.pack("<I", orig), struct.pack("<I", bsym[name] | 1)))
