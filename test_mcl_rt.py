"""MCL blob runtime (milestone M2) under Unicorn: src/mcl_bb/mcl_rt.cpp, mcl_blob.ld, patches/mclblob.py, the hook in src/cpu.c.

Real code runs for: cpu.c (label constructor -> start() -> blob boot -> label text), the blob runtime (data copy, bss zero, heap, static
constructors, self test, malloc / free). Checked: image layout, the allocator-top patch, a fresh boot on garbage SDRAM, exact bounds of the
copy / zeroing, constructor + virtual call, heap coalescing, refusal when the SDRAM region is not reserved (nothing written), status texts.
NOT checked (hardware only): that the unit boots the image, that the reserved region is really unused by the stock firmware, SDRAM timing.
"""
import glob
import struct
import subprocess
import sys

from unicorn.arm_const import UC_ARM_REG_R0

from emu import Emu

IMG = glob.glob("out/*+cave+mclblob/BLACKBOX.bin")[0]
BASE, BLOB, RAM, RAM_END = 0x08040000, 0x08120000, 0xC3F80000, 0xC4000000
BKP = 0x38800920
bsym = {l.split()[2]: int(l.split()[0], 16) for l in subprocess.run(["arm-none-eabi-nm", "out/mcl.elf"], capture_output=True, text=True, check=True).stdout.splitlines()}
image = open(IMG, "rb").read()
stock = open("firmware/BLACKBOX-3.1.9.bin", "rb").read()
fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


def make(image_patch=None, top=0xC3F00000):
    e = Emu(IMG, "out/cave.elf")
    if image_patch:
        image_patch(e.uc)
    e.uc.mem_map(0xE0000000, 0x100000)
    e.uc.mem_map(0x38800000, 0x1000)
    e.uc.mem_map(0x58024000, 0x1000)
    e.uc.mem_map(RAM, RAM_END - RAM)
    e.uc.mem_write(RAM, b"\xa5" * (RAM_END - RAM))                  # SDRAM is garbage at power-up
    e.uc.mem_write(0x38800000, b"\x5a" * 0x1000)                    # so is backup SRAM (or stale from the last boot)
    e.w32(0x24000088, top)                                          # the stock allocator after boot: top has moved down
    e.w32(0x2400008C, 0xC0000000 + 40 * 1024 * 1024)
    e.stub(0x080A3DEC)
    e.uc.mem_write(0x24060000, bytes(0x60))
    e.uc.mem_write(0x24060000 + 0x31, e.cstr(0x080CF290).encode() + b"\0")
    e.uc.mem_write(0x2405FFD0, b"\xa5" * 36)                        # patch RAM is garbage at power-up: cpu.c start() runs
    return e


def label(e):
    return e.cstr(0x24060000 + 0x31)


def boot(e):
    e.call("cpu_text_b", 0x24060000, 0x080CF290)
    return label(e)


# --- layout of the image
blob = open("out/mcl.bin", "rb").read()
check("blob sits at 0x08120000, entry table first", image[BLOB - BASE:BLOB - BASE + len(blob)] == blob and bsym["mcl_entry_table"] == BLOB)
check("blob fits the proven window (ends below 0x08180000)", BLOB + len(blob) <= 0x08180000, hex(BLOB + len(blob)))
cave_end = 0x080F1E78 + len(open("out/cave.bin", "rb").read())
check("main cave ends below the blob; 0xFF between", cave_end <= BLOB and image[cave_end - BASE:BLOB - BASE] == b"\xff" * (BLOB - cave_end), hex(cave_end))
d = [i for i in range(len(stock)) if stock[i] != image[i]]
top_off = 0x080F1E08 - BASE
check("allocator initial top 0xc4000000 -> 0xc3f80000 in the stock .data image, its neighbour (floor) untouched",
      struct.unpack_from("<II", image, top_off) == (RAM, 0xC0000000) and struct.unpack_from("<II", stock, top_off) == (0xC4000000, 0xC0000000))
check("the blob's data sits in the reserved region, bss and heap inside it", RAM <= bsym["__data_start"] < bsym["__bss_end"] < bsym["__heap_limit"] == RAM_END,
      [hex(bsym[k]) for k in ("__data_start", "__bss_end", "__heap_limit")])

# --- fresh boot
e = make()
lab = boot(e)
check("label gets ' Mok' and stays within 30 bytes", lab.endswith(" Mok") and len(lab) <= 30, lab)
ds, de, bs, be, hs = (bsym[k] for k in ("__data_start", "__data_end", "__bss_start", "__bss_end", "__heap_start"))
lma = bsym["__data_lma"]
mem = lambda a, n: bytes(e.uc.mem_read(a, n))
check(".data copied from flash exactly", mem(ds, de - ds) == bytes(image[lma - BASE:lma - BASE + de - ds]), (hex(ds), hex(de)))
bssm = mem(bs, be - bs)
check(".bss cleared of the garbage (only what the runtime itself sets - heap bookkeeping - is non-zero), .data starts at the region start",
      b"\xa5\xa5\xa5\xa5" not in bssm and sum(1 for b in bssm if b) < 64 and ds == RAM, sum(1 for b in bssm if b))
check("the heap is not zeroed by boot beyond what malloc handed out (garbage left above .bss)", mem(be + 4096, 64) == b"\xa5" * 64)
check("one static constructor ran, once", e.r32(bsym["_ZN5Probe9ctor_runsE"]) == 1)
s = [e.r32(BKP + 4 * i) & 0xFFFFFFFF for i in range(4)]
check("status words in backup SRAM: magic, ok, 1 constructor, bss size", s == [0x4D434C31, 1, 1, be - bs], [hex(x) for x in s])
check("self test passed (no bad bits)", s[1] >> 8 == 0)

# --- heap (real malloc / free of the blob)
def call(e, name, *a):
    e.call(bsym[name], *a)
    return e.uc.reg_read(UC_ARM_REG_R0)


total = e.r32(bsym["_ZL16heap_bytes_total"]) & 0xFFFFFFFF
check("heap spans __heap_start .. __heap_limit", total == RAM_END - ((hs + 15) & ~15) - 16, (total, hex(hs)))
ps = [call(e, "malloc", n) for n in (100, 5000, 16, 777, 40000)]
check("malloc: distinct, 16-byte aligned, inside the heap", len(set(ps)) == 5 and all(p % 16 == 0 and hs <= p < RAM_END for p in ps), [hex(p) for p in ps])
check("malloc beyond the heap gives 0", call(e, "malloc", 0x100000) == 0)
for p in (ps[1], ps[3], ps[0], ps[4], ps[2]):                      # free in a scrambled order
    call(e, "free", p)
check("everything freed: the heap is one block again (a near-total malloc works)", call(e, "malloc", total - 64) != 0)
check("free(0) is harmless", call(e, "free", 0) is not None)

# --- refusals: nothing is touched when the region is not reserved
def stock_top(u):
    u.mem_write(0x080F1E08, struct.pack("<I", 0xC4000000))           # image without the allocator patch

e = make(stock_top)
lab = boot(e)
check("allocator patch missing: label ' M2', region and .bss untouched", lab.endswith(" M2") and mem(RAM, 4096) == b"\xa5" * 4096 and mem(bs, 64) == b"\xa5" * 64, lab)
e = make(top=0xC3FC0000)                                             # top above the reserved region: stock owns part of it
lab = boot(e)
check("allocator top inside the region: label ' M2', nothing written", lab.endswith(" M2") and mem(ds, 16) == b"\xa5" * 16, lab)

# --- status text formats
def status_text(e):
    e.uc.mem_write(0x24061000, bytes(16))
    e.call(bsym["mcl_status_text"], 0x24061000)
    return e.cstr(0x24061000)


e = make()
boot(e)
check("status text after a good boot is 'Mok'", status_text(e) == "Mok")
e.w32(BKP + 4, 3 | (0x0a << 8))
check("self test failure reads 'M30a' (code 3, bad bits 0x0a)", status_text(e) == "M30a", status_text(e))
e.w32(BKP + 4, 4)
check("region too small reads 'M4'", status_text(e) == "M4", status_text(e))
e.uc.mem_write(BKP, bytes(16))
check("no status yet reads 'M-'", status_text(e) == "M-", status_text(e))

# --- compact label (hardware test images): the label is the status text only
e = make()
e.w32(bsym["mcl_entry_table"] + 20, 1)                           # the entry table's `compact` word (flash is writable in the emulator)
lab = boot(e)
check("compact flag: the version label is just 'Mok'", lab == "Mok", lab)

# --- the rest of the mod is unchanged: no blob (flag 0) means no boot, no label text, nothing written
e = make()
e.uc.mem_write(e.sym["mcl_entry"], bytes(4))
lab = boot(e)
check("mcl_entry = 0: no ' M' text and the region is not touched", " M" not in lab.replace(" 41M", "") and mem(RAM, 64) == b"\xa5" * 64 and e.r32(BKP) & 0xFFFFFFFF != 0x4D434C31, lab)

print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
