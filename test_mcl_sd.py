"""MCL's SD layer (milestone M7) under Unicorn, with the STOCK FatFs running on a RAM disk (sdharness.py): src/mcl_bb/bb_sd.cpp, patches/mclsd.py.

The firmware's own f_mount / f_open / f_read / ... run in the emulator; only the five card-driver entries are Python (a FAT32 image made by mkfs.vfat). The GUI side (blob) and the
streamer side (service) run as two contexts: when the GUI side blocks on its semaphore the test runs the streamer's service, then resumes the GUI.
Checked: the patch site; every operation of the API against the real FatFs (create / write / read / seek / truncate / rename / unlink / mkdir / directory listing / stat); that
the card driver is only ever given the FatFs window as its buffer (never a caller's buffer: the 511-byte rule, with a negative control showing a direct 2 KB read does hit the caller's
buffer); fsck.vfat clean and files readable with mtools afterwards (interchangeable with another FAT reader); handle exhaustion, busy mailbox, timeout, not-ready, a request found before boot.
NOT checked (hardware only): the real streamer task's scheduling, how long calls take on the card, cache behaviour of the real memory.
"""
import os
import random
import shutil
import struct
import subprocess
import sys

from unicorn.arm_const import (UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5,
                               UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP)

import sdharness as H
from emu import RET, STACK
from mcl_rig import Rig, emulate_primask, syms

NAMES = "solo slice duck chord cond filter cpu od comp seqfix fx2 munchi looper clkmon cave mclblob mclsd".split()
subprocess.run(["tools/mcl/build_blob.sh"], check=True, capture_output=True, env=dict(os.environ, M3TEST="", M4TEST="", M5TEST="", SDSPY="", COMPACT="", SDTEST="1"))
subprocess.run(["python3", "patch.py"] + NAMES, check=True, capture_output=True)
os.makedirs("out/mclsd-test", exist_ok=True)
shutil.copy("out/" + "+".join(NAMES) + "/BLACKBOX.bin", "out/mclsd-test/BLACKBOX.bin")
shutil.copy("out/mcl.elf", "out/mclsd-test/mcl.elf")
fails = 0


def check(label, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + label + (f"  [{detail}]" if not cond else ""))
    fails += not cond


img = open("out/mclsd-test/BLACKBOX.bin", "rb").read()
stock = open("firmware/BLACKBOX-3.1.9.bin", "rb").read()
b = syms("out/mclsd-test/mcl.elf")
BASE = 0x08040000
changed = [a for a in range(0x80743E8 - BASE, 0x80743E8 - BASE + 4) if img[a] != stock[a]]
h1, h2 = struct.unpack("<HH", img[0x80743E8 - BASE:0x80743E8 - BASE + 4])
s, j1, j2 = (h1 >> 10) & 1, (h2 >> 13) & 1, (h2 >> 11) & 1
off = (s << 24) | ((1 - (j1 ^ s)) << 23) | ((1 - (j2 ^ s)) << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1)
off -= (1 << 25) if s else 0
check("the streamer loop's queue call goes to mcl_sd_pump (and only those four bytes of the loop changed)", 0x80743E8 + 4 + off == b["mcl_sd_pump"] & ~1 and img[0x80743EC - BASE:0x80743F4 - BASE] == stock[0x80743EC - BASE:0x80743F4 - BASE])

# ---- the rig: the unit's image with the blob, a FAT image behind the stock FatFs
os.makedirs("out/sd", exist_ok=True)
rnd = random.Random(7)
open("/tmp/hello.txt", "wb").write(b"hello from the card\n" * 50)
H.make_image("out/sd/t.img", files=[("/tmp/hello.txt", "::/Mcl/hello.txt")])
rg = Rig("out/mclsd-test")
e = rg.e
uc = e.uc
emulate_primask(rg)
REQ = next(v for k, v in b.items() if k.endswith("3req"))
READY = next(v for k, v in b.items() if k.endswith("5ready") and "sd" not in k.lower() or k.endswith("5ready"))
# the blob has several statics called `ready`: the SD one is the one holding "SDM1" after boot
rg.call("mcl_boot")
cands = [v for k, v in b.items() if k.endswith("5ready")]
READY = next((v for v in cands if e.r32(v) & 0xFFFFFFFF == 0x53444D31), None)
check("boot: the SD layer is ready (flag SDM1)", READY is not None)
card = H.Card("out/sd/t.img", emu=e)

Q_CREATE, Q_SEND, Q_RECEIVE = 0x080883BE, 0x08088402, 0x08088756
blocked = [False]
sends = [0]
waits = [0]
e.stub(Q_CREATE, value=0x24050000)
e.stub(Q_SEND, lambda: sends.__setitem__(0, sends[0] + 1), value=1)


def q_receive():
    ticks = e.arg(2)
    if ticks == 50:                                              # the GUI side sleeping until the streamer is done
        waits[0] += 1
        blocked[0] = True
        e.ret(1)
        uc.emu_stop()
    else:
        e.ret(0)                                                 # the streamer's own non-blocking take: queue empty


e.stub(Q_RECEIVE, q_receive)
REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
        UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC]
serviced = [0]
stream_stack = STACK - 0x8000
pump_calls = [0]


def service():
    """the streamer's loop iteration: the patched call runs mcl_sd_pump (service, then the stock queue take)"""
    saved = [uc.reg_read(r) for r in REGS]
    for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), (0x24060000, 0, 0, 0)):
        uc.reg_write(r, v)
    uc.reg_write(UC_ARM_REG_SP, stream_stack)
    uc.reg_write(UC_ARM_REG_LR, RET | 1)
    pump_calls[0] += 1
    uc.emu_start(b["mcl_sd_pump"] | 1, RET, count=50_000_000)
    for r, v in zip(REGS, saved):
        uc.reg_write(r, v)


def gui(name, *args, max_loops=20000):
    """run a blob function as the GUI task; whenever it blocks, the streamer gets a turn"""
    for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
        uc.reg_write(r, v & 0xFFFFFFFF)
    uc.reg_write(UC_ARM_REG_SP, STACK)
    uc.reg_write(UC_ARM_REG_LR, RET | 1)
    pc = b[name] | 1
    for _ in range(max_loops):
        blocked[0] = False
        uc.emu_start(pc, RET, count=50_000_000)
        if not blocked[0]:
            return uc.reg_read(UC_ARM_REG_R0)
        service()
        pc = uc.reg_read(UC_ARM_REG_PC) | 1
    raise AssertionError("did not finish")


def signed(v):
    return v - (1 << 32) if v & 0x80000000 else v


def cstring(addr, n=64):
    return bytes(uc.mem_read(addr, n)).split(b"\0")[0].decode()


uc.mem_map(0x30000000, 0x400000)                                  # caller-side scratch (strings, data buffers)
S1, S2, BUFA, BUFB = 0x30000000, 0x30001000, 0x30100000, 0x30200000


def put(addr, data):
    uc.mem_write(addr, data)


def path(s, addr=S1):
    put(addr, s.encode() + b"\0")
    return addr


# ---- stat / listing of what mkfs + mtools made
ent = 0x30002000
check("stat of an existing file: size 1000", gui("bb_sd_stat", path("/Mcl/hello.txt"), ent) == 0 and struct.unpack("<Q", bytes(uc.mem_read(ent, 8)))[0] == 1000 and cstring(ent + 9) == "hello.txt", cstring(ent + 9))
check("stat of a missing file: FR_NO_FILE (4)", gui("bb_sd_stat", path("/Mcl/nothere.bin"), ent) == 4)
d = signed(gui("bb_sd_opendir", path("/Mcl")))
names = []
while d >= 0:
    r = gui("bb_sd_readdir", d, ent)
    nm = cstring(ent + 9)
    if r != 0 or not nm:
        break
    names.append((nm, bytes(uc.mem_read(ent + 8, 1))[0]))
check("directory listing: hello.txt only", names == [("hello.txt", 0x20)], names)
check("closedir ok, handle freed", gui("bb_sd_closedir", d) == 0)

# ---- create, write, read back (all through the 511-byte rule)
data = bytes(rnd.randrange(256) for _ in range(300_000))
put(BUFA, data)
card.log.clear()
h = signed(gui("bb_sd_open", path("/Mcl/big.bin"), H.__dict__.get("FA", 0) or (0x08 | 0x02 | 0x01)))
check("open for create+write: handle 0", h == 0, h)
w = signed(gui("bb_sd_write", h, BUFA, len(data)))
check("write 300,000 bytes in one call: all written", w == len(data), w)
check("close ok", gui("bb_sd_close", h) == 0)
put(BUFB, bytes(len(data) + 16))
h = signed(gui("bb_sd_open", path("/Mcl/big.bin"), 1))
got = signed(gui("bb_sd_read", h, BUFB, len(data) + 16))
check("read back: 300,000 bytes (a longer request stops at the end), identical", got == len(data) and bytes(uc.mem_read(BUFB, len(data))) == data, got)
reads = [t for t in card.log if t[0] == "r"]
writes = [t for t in card.log if t[0] == "w"]
check("the card driver was only ever given the FatFs window as its buffer (%d reads, %d writes, all at FS+0x3c)" % (len(reads), len(writes)), all(t[3] == card.FS + 0x3C for t in card.log) and len(card.log) > 1000, {hex(t[3]) for t in card.log})
# seek / tell / partial read
check("seek to 123,457 and read 700 bytes", gui("bb_sd_seek", h, 0, 123457, 0) == 0 and signed(gui("bb_sd_read", h, BUFB, 700)) == 700 and bytes(uc.mem_read(BUFB, 700)) == data[123457:123457 + 700])
check("close", gui("bb_sd_close", h) == 0)

# ---- the negative control: a 2 KB read straight through FatFs hits the caller's buffer (this is what the rule avoids)
FIL, WIN = 0x24042000, 0x24048000
put(WIN - 0x100, b"0:/Mcl/big.bin\0")
uc.mem_write(FIL, bytes(0x60))
card.call(H.F_OPEN, FIL, WIN - 0x100, 1)
card.log.clear()
card.call(H.F_READ, FIL, 0x24049000, 2048, WIN + 0x200)
check("control: a direct 2048-byte f_read does give the card the caller's buffer (so the rule is needed)", any(t[3] == 0x24049000 for t in card.log), [hex(t[3]) for t in card.log])
card.call(H.F_CLOSE, FIL)

# ---- append, truncate, rename, unlink, mkdir
h = signed(gui("bb_sd_open", path("/Mcl/log.txt"), 0x30 | 2))
check("append mode open (creates)", h >= 0, h)
put(S2, b"line one\n")
gui("bb_sd_write", h, S2, 9)
gui("bb_sd_close", h)
h = signed(gui("bb_sd_open", path("/Mcl/log.txt"), 0x30 | 2))
put(S2, b"line two\n")
gui("bb_sd_write", h, S2, 9)
gui("bb_sd_close", h)
h = signed(gui("bb_sd_open", path("/Mcl/log.txt"), 1))
n = signed(gui("bb_sd_read", h, BUFB, 100))
check("append twice: both lines in order", n == 18 and bytes(uc.mem_read(BUFB, 18)) == b"line one\nline two\n", n)
gui("bb_sd_close", h)
h = signed(gui("bb_sd_open", path("/Mcl/log.txt"), 3))
gui("bb_sd_seek", h, 0, 9, 0)
check("truncate at 9 bytes", gui("bb_sd_truncate", h) == 0)
gui("bb_sd_close", h)
check("stat after truncate: size 9", gui("bb_sd_stat", path("/Mcl/log.txt"), ent) == 0 and struct.unpack("<Q", bytes(uc.mem_read(ent, 8)))[0] == 9)
check("mkdir /Mcl/Projects", gui("bb_sd_mkdir", path("/Mcl/Projects")) == 0)
check("mkdir again: FR_EXIST (8)", gui("bb_sd_mkdir", path("/Mcl/Projects")) == 8)
put(S2, b"/Mcl/Projects/moved.txt\0")
check("rename into the new directory", gui("bb_sd_rename", path("/Mcl/log.txt"), S2) == 0 and gui("bb_sd_stat", S2, ent) == 0)
check("old name is gone", gui("bb_sd_stat", path("/Mcl/log.txt"), ent) == 4)
check("unlink a file", gui("bb_sd_unlink", S2) == 0 and gui("bb_sd_stat", S2, ent) == 4)
check("unlink an empty directory", gui("bb_sd_unlink", path("/Mcl/Projects")) == 0)

# ---- long names and a directory with many entries (MCL's project / grid files)
for i in range(60):
    h = signed(gui("bb_sd_open", path("/Mcl/Project%03d.mcl" % i), 8 | 2))
    if h < 0:
        break
    put(S2, bytes([i]) * 100)
    gui("bb_sd_write", h, S2, 100)
    gui("bb_sd_close", h)
d = signed(gui("bb_sd_opendir", path("/Mcl")))
cnt = 0
seen = set()
while True:
    r = gui("bb_sd_readdir", d, ent)
    nm = cstring(ent + 9)
    if r != 0 or not nm:
        break
    seen.add(nm)
    cnt += 1
gui("bb_sd_closedir", d)
check("listing a directory of 62 entries (long names): all there", cnt == 62 and "Project059.mcl" in seen and "big.bin" in seen and "hello.txt" in seen, (cnt, sorted(seen)[:3]))

# ---- handles and the mailbox
hs = [signed(gui("bb_sd_open", path("/Mcl/Project%03d.mcl" % i), 1)) for i in range(5)]
check("four files at once, a fifth open is refused (-SD_BUSY)", hs[:4] == [0, 1, 2, 3] and hs[4] == -100, hs)
check("bad handle: SD_BAD_HANDLE", gui("bb_sd_close", 9) == 103 and signed(gui("bb_sd_read", 9, BUFB, 10)) == -103)
for x in hs[:4]:
    gui("bb_sd_close", x)
check("handles are free again after close", signed(gui("bb_sd_open", path("/Mcl/Project001.mcl"), 1)) == 0)
gui("bb_sd_close", 0)
check("opening a file that is not there: -FR_NO_FILE", signed(gui("bb_sd_open", path("/Mcl/none.bin"), 1)) == -4)
# busy: a request is posted and not yet served
uc.mem_write(REQ, struct.pack("<I", 1))
check("a posted request not yet served: the next call says SD_BUSY", gui("bb_sd_mkdir", path("/Mcl/x")) == 100)
uc.mem_write(REQ, struct.pack("<I", 0))
# not ready (before the blob's boot): nothing is touched
uc.mem_write(READY, struct.pack("<I", 0))
check("ready flag clear: calls say SD_NOT_READY, and the streamer's service ignores a posted request",
      gui("bb_sd_mkdir", path("/Mcl/y")) == 102)
uc.mem_write(REQ, struct.pack("<I", 1))
before = len(card.log)
service()
check("... a request found while not ready is not executed", len(card.log) == before and struct.unpack("<I", bytes(uc.mem_read(REQ, 4)))[0] == 1)
uc.mem_write(REQ, struct.pack("<I", 0))
uc.mem_write(READY, struct.pack("<I", 0x53444D31))
# timeout: the streamer never runs
def never(name, *a):
    for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), a):
        uc.reg_write(r, v & 0xFFFFFFFF)
    uc.reg_write(UC_ARM_REG_SP, STACK)
    uc.reg_write(UC_ARM_REG_LR, RET | 1)
    from unicorn.arm_const import UC_ARM_REG_R0 as R0
    t = {"cyc": 0}
    uc.emu_start(b[name] | 1, RET, count=5_000_000)
    return uc.reg_read(R0)
DWT = 0xE0001004
cyc = {"v": 0}
from unicorn import UC_HOOK_MEM_READ
def dwt_read(u, a, addr, size, v, _):
    cyc["v"] += 400_000_00 // 10                                 # every read of the cycle counter: 10 ms at 399 MHz-ish (SYST reload not set here: scaled below)
    u.mem_write(DWT, struct.pack("<I", cyc["v"] & 0xFFFFFFFF))
uc.hook_add(UC_HOOK_MEM_READ, dwt_read, begin=DWT, end=DWT + 3)
uc.mem_write(0xE000E014, struct.pack("<I", 399_000 - 1))
q_wait_stub = e.stubs[Q_RECEIVE]
def q_receive_nowake():
    e.ret(0 if e.arg(2) != 50 else 0)
e.stubs[Q_RECEIVE] = q_receive_nowake
r = never("bb_sd_mkdir", path("/Mcl/z"), 0, 0, 0)
check("the streamer never runs: SD_TIMEOUT, counted, the request is marked abandoned", r == 101 and struct.unpack("<I", bytes(uc.mem_read(REQ + 4, 4)))[0] == 1, r)
e.stubs[Q_RECEIVE] = q_wait_stub
service()
check("a late completion of an abandoned request frees the mailbox", struct.unpack("<I", bytes(uc.mem_read(REQ, 4)))[0] == 0)
check("... and the work was done (the directory exists)", gui("bb_sd_stat", path("/Mcl/z"), ent) == 0)

# ---- the test bench on the MCL page (SD_TEST): tiles B1 list, B2 read, B3 write; B4 counters
src = open("src/mcl_bb/glcdfont.h").read()
FONT = [int(t, 16) for t in src[src.index("{") + 1:src.rindex("}")].replace("//", " ").replace(",", " ").split() if t.startswith("0x")]


def screen():
    gui("bb_ui_refresh")
    gui("bb_ui_sig")
    fb = bytes(uc.mem_read(gui("bb_ui_fb"), 512))
    rows = []
    for row in range(4):
        t = ""
        for c in range(21):
            cols = [sum((fb[c * 6 + i + 128 * ((row * 8 + j) >> 3)] >> ((row * 8 + j) & 7) & 1) << j for j in range(7)) for i in range(5)]
            t += next((chr(k) for k in range(32, 127) if [FONT[k * 5 + i] & 0x7f for i in range(5)] == cols), "?")
        rows.append(t.rstrip())
    return rows


def tile(i):
    gui("bb_ui_input", 2, i, 0)
    gui("bb_ui_input", 3, i, 0)


sample = bytes(rnd.randrange(256) for _ in range(600_000))
put(BUFA, sample)
h = signed(gui("bb_sd_open", path("/SAMPLE.WAV"), 8 | 2))
gui("bb_sd_write", h, BUFA, len(sample))
gui("bb_sd_close", h)
tile(0)
rows = screen()
check("B1 list: the root holds 1 file and 1 folder; the biggest file size is shown", rows[0] == "LS F1 D1 BIG 585K", rows)
tile(1)
rows = screen()
check("B2 read: 256K read twice (4 KB requests, then 100-byte requests), checksums equal, speed shown", rows[1].startswith("RD 256K ") and rows[1].endswith("K/s") and "MISMATCH" not in rows[1], rows)
n_before = len([t for t in card.log if t[0] == "w"])
card.log.clear()
tile(2)
rows = screen()
check("B3 write: 128K written, read back and verified, file deleted; all four lines readable at once", rows[0] == "LS F1 D1 BIG 585K" and rows[1].startswith("RD 256K ") and rows[2].startswith("WR 128K ") and " w" in rows[2] and " r" in rows[2] and rows[3] == "VERIFY OK 0 DEL OK" and all(len(r) <= 21 for r in rows), rows)
check("... again only the FatFs window ever reached the card driver", all(t[3] == card.FS + 0x3C for t in card.log) and len(card.log) > 500)
check("... and the temporary file is really gone", gui("bb_sd_stat", path("/BBSD.TMP"), ent) == 4)
tile(3)
rows = screen()
check("B4: counters page: requests counted, the one deliberate timeout of the test above, its 30 s wait as the worst wait, the streamer gap lines present", rows[0].startswith("REQ") and rows[0].endswith("TMO  1") and rows[1].startswith("STREAMER GAP MS") and rows[2].startswith("GAPS OVER 100MS") and "30015" in rows[3], rows)
tile(3)
# a failing card: write errors reach the screen

# ---- the volume afterwards: fsck and mtools
card.save("out/sd/after.img")
fsck = subprocess.run(["fsck.vfat", "-n", "out/sd/after.img"], capture_output=True, text=True)
check("fsck.vfat: the volume is clean after all of that", fsck.returncode == 0, fsck.stdout[-300:])
out = subprocess.run(["mcopy", "-i", "out/sd/after.img", "::/Mcl/big.bin", "-"], capture_output=True).stdout
check("mtools reads back the 300,000-byte file we wrote, identical (the card is readable by another FAT implementation)", out == data, len(out))
out = subprocess.run(["mcopy", "-i", "out/sd/after.img", "::/Mcl/hello.txt", "-"], capture_output=True).stdout
check("the file that was already on the card is untouched", out == b"hello from the card\n" * 50)
ls = subprocess.run(["mdir", "-i", "out/sd/after.img", "::/Mcl"], capture_output=True, text=True).stdout
check("long file names are real long names for mtools too", "Project059.mcl" in ls, ls[-200:])

print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
