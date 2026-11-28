"""Flash bank 2 pairing (src/b2.c, patches/b2guard.py), under Unicorn.

Real code runs for: b2_gate (the verdict, the sum, the ECC scan, the machines' ITCM copy), every guard stub of every hook that jumps into bank 2, the bank 1 stubs of the functions the
core calls in the machines (tools/b2link.py). Three payloads: the good one (paired), one with a flipped byte, and none (a bank 1 file alone on the card).
NOT checked: the flash itself (the ECC flags, the card loader's programming: hardware), which hooks the stock firmware reaches when.
"""
import os
import struct
import sys

sys.path.insert(0, "patches")
sys.argv = ["x"]
import b2guard
from emu import Emu
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_SP

import subprocess
SETS = "solo slice duck chord cond filter cpu od comp seqfix fx2 munchi looper lira lirarec lirasave cave".split()
subprocess.run([sys.executable, "patch.py"] + SETS, check=True, capture_output=True)
FULL = open("out/" + "+".join(SETS) + "/FULL.bin", "rb").read()
fails = 0


def check(msg, ok):
    global fails
    print(("PASS " if ok else "FAIL ") + msg)
    fails += not ok


def emu_with(payload_mod):
    img = bytearray(FULL)
    payload_mod(img)
    path = "out/_b2test.bin"
    open(path, "wb").write(img)
    e = Emu(path, "out/cave.elf")
    for a, n in ((0x38800000, 0x1000), (0xE0000000, 0x100000), (0x58024000, 0x1000)):
        try:
            e.uc.mem_map(a, n)
        except Exception:
            pass
    n = e.sym["__itcm2_end"] - e.sym["__itcm2_start"]
    e.uc.mem_write(e.sym["__itcm2_start"], bytes(n))      # (the harness copied the machines' ITCM code in: the real check does it)
    e.uc.mem_write(0x100, bytes(8))                       # forget the verdict the harness set: the real decision runs
    return e


def banks(img):
    return 0x08100000 - 0x08040000


def corrupt(img):
    img[banks(img) + 0x2000] ^= 1


def blank(img):
    img[banks(img):] = b"\xff" * (len(img) - banks(img))


for label, mod, want in (("good payload", lambda i: None, 1), ("a payload with one flipped byte", corrupt, 0), ("no payload (BBEXTRA.BIN not written yet)", blank, 0)):
    e = emu_with(mod)
    r = e.call("b2_gate")
    ret = e.uc.reg_read(UC_ARM_REG_R0)
    check(f"{label}: b2_gate says {want}", ret == want)
    check(f"{label}: the verdict is kept", e.r32(0x100) == (0x42325041 if want else 0x4232554E))
    if want:
        h = struct.unpack("<8I", FULL[banks(FULL):banks(FULL) + 32])
        check(f"{label}: the machines' ITCM code was copied in", bytes(e.uc.mem_read(h[6], h[5])) == FULL[banks(FULL) + h[4]:banks(FULL) + h[4] + h[5]])
    else:
        check(f"{label}: no machine code was copied into ITCM", bytes(e.uc.mem_read(e.sym["__itcm2_start"], 256)) == bytes(256))

    # every guard stub: paired -> the machine function; unpaired -> what stock does at that place, and nothing in bank 2 runs
    st = b2guard.stubs()
    bad = []
    checked = 0
    mine = {a for s in ('looper', 'lira', 'lirarec', 'lirasave') for a, o, n in __import__(s).PATCHES}
    for site, (sa, sb) in sorted(st.items()):
        if site not in mine:
            continue
        checked += 1
        e2 = emu_with(mod)
        e2.call("b2_gate")
        hits = []
        stop = {}
        o, n = b2guard.all_sites()[site]
        fb = b2guard._site(site, o, n)
        stop = {"expect": (fb[1] & ~1) if fb[0] == "jump" else 0x0807FFF0 if fb[2] == "ret" else fb[2] & ~1}
        uc = e2.uc
        lo, hi = 0x08100000, 0x081C0000
        seen = {}

        def hook(uc, addr, size, _):
            if lo <= addr < hi or e.sym['__itcm2_start'] <= addr < e.sym['__itcm2_end']:
                seen.setdefault("b2", addr)
            if addr == stop["expect"]:
                seen["fallback"] = True
                uc.emu_stop()
        h = uc.hook_add(UC_HOOK_CODE, hook)
        uc.reg_write(UC_ARM_REG_SP, 0x2407F000)
        uc.reg_write(UC_ARM_REG_LR, 0x0807FFF0 | 1)
        try:
            uc.emu_start(sa | 1, 0x0807FFF0, count=400)
        except Exception as ex:
            seen["err"] = str(ex)
        uc.hook_del(h)
        if uc.reg_read(UC_ARM_REG_PC) == 0x0807FFF0 and stop['expect'] == 0x0807FFF0:
            seen['fallback'] = True
        if not want and ("b2" in seen or "fallback" not in seen):
            bad.append((hex(site), seen))
        if want and "b2" not in seen:
            bad.append((hex(site), "paired but never reached bank 2", seen))
    check(f"{label}: {checked} guard stubs do what they should", not bad)
    for b in bad[:5]:
        print("   ", b)

for sym_name, expect in (("looper_ready", 0), ("looper_page_next", 0xFFFFFFFF), ("looper_page_on", 0)):
    e = emu_with(blank)
    e.call(sym_name, 0)
    check(f"unpaired: the core's call {sym_name}() returns {expect:#x}", e.uc.reg_read(UC_ARM_REG_R0) == expect)
e = emu_with(blank)
e.stub(0x080518F0, value=0x5A5A)
e.call("looper_in", 0, 0, 0)
check("unpaired: looper_in (the input stage's replaced tail call) runs stock's", e.uc.reg_read(UC_ARM_REG_R0) == 0x5A5A)
print(f"{fails} failure(s)")
sys.exit(1 if fails else 0)
