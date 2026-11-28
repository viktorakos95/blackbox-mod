"""Guards for the hooks that jump into flash bank 2 (src/b2.h).

A hook whose target is machine code lives in bank 2, which may be missing (no BBEXTRA.BIN on the card, or not written yet) or the wrong one. So such a hook never
jumps there directly: it jumps to a stub in bank 1 (the `b2_stub_area` of src/b2.c, 64 bytes a site) that asks b2_gate() and, when bank 2 is not this image's payload, does what
stock does at that place:
  BL  that replaced a BL to X            -> branch to X (the call, as stock made it)
  BL  that replaced one instruction      -> run that instruction, return
  B.W that replaced a B.W to X           -> branch to X
  B.W that replaced a function's first instruction -> run it, go on at the next one
  a table word (vtable / vector)         -> the old word's function
`transform(patches)` rewrites a set's patch list that way (hook sites in the stock image only; patches into the cave or the payload are left alone). The stub bytes are
ordinary patches, so the web patcher needs no code for this. Every site has its own slot, numbered over ALL the sets, so a stub's place does not depend on which sets are chosen.
"""
import importlib
import os
import struct
import subprocess
import sys
import tempfile

from thumb import ROOT, bl, symbols

B2_BASE, B2_TOP = 0x08100000, 0x081C0000
STOCK_END = 0x080F1E78
SLOT = 64
_STOCK = open(os.path.join(ROOT, "firmware", "BLACKBOX-3.1.9.bin"), "rb").read()


def _dec(at, b):
    """A BL or B.W of 4 bytes -> ('BL' | 'B.W', target); anything else -> (None, None)."""
    if len(b) != 4:
        return None, None
    h1, h2 = struct.unpack("<HH", b)
    if h1 & 0xF800 != 0xF000 or h2 & 0x8000 == 0:
        return None, None
    kind = "BL" if h2 & 0x4000 and h2 & 0x1000 else "B.W" if h2 & 0x1000 and not h2 & 0x4000 else None
    if kind is None:
        return None, None
    s, j1, j2 = (h1 >> 10) & 1, (h2 >> 13) & 1, (h2 >> 11) & 1
    i1, i2 = 1 - (j1 ^ s), 1 - (j2 ^ s)
    off = (s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1)
    if s:
        off -= 1 << 25
    return kind, at + 4 + off


def _site(addr, old, new):
    """The guard this patch needs: None, or (kind, fallback) with fallback ('jump', X) / ('replay', old bytes, 'ret' | resume address)."""
    if addr >= STOCK_END or len(new) != 4 or len(old) != 4:
        return None
    k, t = _dec(addr, new)
    if k and B2_BASE <= t < B2_TOP:
        ok, ot = _dec(addr, old)
        if ok == k and not (ot is None):
            return ("jump", ot | 1)
        # run the instructions the hook displaced: whole instructions, so a patch that cuts one in half (4 bytes over a 2 + 4 byte pair) replays the rest of it too
        o0 = addr - 0x08040000
        pos = 0
        while pos < 4:
            h1 = struct.unpack("<H", _STOCK[o0 + pos:o0 + pos + 2])[0]
            assert h1 & 0xF800 != 0xF000 and h1 & 0xFF7F != 0xF85F and not (h1 & 0xF800 == 0x4800) and not 0xD000 <= h1 & 0xFF00 <= 0xDD00 and h1 & 0xF800 != 0xE000 and h1 & 0xF500 != 0xB100, \
                f"the instruction at {addr + pos:#x} cannot be replayed (a branch or a pc-relative load)"
            pos += 4 if h1 & 0xF800 in (0xE800, 0xF000, 0xF800) else 2
        return ("replay", _STOCK[o0:o0 + pos], "ret" if k == "BL" else addr + pos)
    w = struct.unpack("<I", new)[0]
    if B2_BASE <= (w & ~1) < B2_TOP:
        return ("jump", struct.unpack("<I", old)[0] | 1)
    return None


_sites = None


def all_sites():
    """{site address: (old, new)} over ALL the sets (those whose MCL blob is missing excepted)."""
    global _sites
    if _sites is None:
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        import patchsets as PS
        sites = {}
        argv = sys.argv
        for s in PS.CANON:
            if s == "cave":
                continue
            if PS.SETS[s].get("mcl") and not os.path.exists(os.path.join(ROOT, "out", "mcl.bin")):
                continue
            sys.argv = ["patch.py", "lirasave"] if s == "mclsd" else ["patch.py"]
            for a, o, n in importlib.import_module(s).PATCHES:
                if _site(a, o, n):
                    sites[a] = (o, n)
        sys.argv = argv
        _sites = sites
    return _sites


def _all_sites():
    return {a: i for i, a in enumerate(sorted(all_sites()))}


def _assemble(sites, area, gate):
    SYM = symbols(os.path.join(ROOT, "out", "cave.elf"))
    out = [".syntax unified\n.thumb\n.text\n"]
    for a in sorted(sites):
        (o, n) = sites[a]
        kind, *fb = _site(a, o, n)
        k, t = _dec(a, n)
        real = t | 1 if k else (struct.unpack("<I", n)[0] | 1)
        out.append(f""".balign {SLOT}
s_{a:08x}:
    push {{r0, r1, r2, r3, r12, lr}}
    vpush {{s0-s15}}
    bl b2_gate
    vpop {{s0-s15}}
    str r0, [sp, #16]
    pop {{r0, r1, r2, r3, r12, lr}}
    cmp r12, #0
    beq 1f
    ldr r12, =0x{real:08x}
    bx r12
1:
""")
        if kind == "jump":
            tgt = fb[0] if fb[0] != 0x08092F5D else SYM["b2load_thunk"] | 1      # stock's file command step: the card loader looks first (src/b2load_thunk.S)
            out.append(f"    ldr r12, =0x{tgt:08x}\n    bx r12\n")
        else:
            old, cont = fb
            out.append("    .short " + ", ".join(f"0x{old[i] | old[i + 1] << 8:04x}" for i in range(0, len(old), 2)) + "\n")
            out.append("    bx lr\n" if cont == "ret" else f"    ldr r12, =0x{cont | 1:08x}\n    bx r12\n")
        out.append("    .ltorg\n")
    return "".join(out)


_built = {}


def stubs():
    """{site address: (stub address, stub bytes)} for all the sites, assembled for the current out/cave.elf."""
    key = (os.path.getmtime(os.path.join(ROOT, "out", "cave.elf")), os.path.exists(os.path.join(ROOT, "out", "mcl.bin")))
    if key in _built:
        return _built[key]
    sites = all_sites()
    slots = _all_sites()
    sym = symbols(os.path.join(ROOT, "out", "cave.elf"))
    area, gate = sym["b2_stub_area"], sym["b2_gate"]
    assert len(sites) * SLOT <= 56 * 64, f"{len(sites)} hook sites need guard stubs: b2_stub_area (src/b2.c) holds 56"
    d = tempfile.mkdtemp()
    src = os.path.join(d, "s.S")
    open(src, "w").write(_assemble(sites, area, gate))
    # one stub per slot: the label of each is at a 64-byte boundary, in slot order of ADDRESS (the slot number is the rank of the site's address)
    subprocess.check_call(["arm-none-eabi-gcc", "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16", "-c", "-o", os.path.join(d, "s.o"), src])
    subprocess.check_call(["arm-none-eabi-ld", f"-Ttext={area:#x}", f"--defsym=b2_gate={gate:#x}", "-o", os.path.join(d, "s.elf"), os.path.join(d, "s.o")])
    subprocess.check_call(["arm-none-eabi-objcopy", "-O", "binary", "-j", ".text", os.path.join(d, "s.elf"), os.path.join(d, "s.bin")])
    blob = open(os.path.join(d, "s.bin"), "rb").read()
    syms = symbols(os.path.join(d, "s.elf"))
    res = {}
    order = sorted(sites)
    for i, a in enumerate(order):
        at = syms[f"s_{a:08x}"]
        nxt = syms[f"s_{order[i + 1]:08x}"] if i + 1 < len(order) else area + len(blob)
        assert nxt - at <= SLOT, f"the guard stub of {a:#x} is {nxt - at} bytes (a slot is {SLOT})"
        res[a] = (area + slots[a] * SLOT, blob[at - area: at - area + SLOT].ljust(SLOT, b"\xff"))
    _built[key] = res
    return res


def transform(patches):
    """A set's patches with each hook that jumps into bank 2 pointed at its stub, and the stubs themselves (patches into b2_stub_area)."""
    out = []
    st = None
    for addr, old, new in patches:
        if _site(addr, old, new):
            st = st or stubs()
            sa, sb = st[addr]
            k, t = _dec(addr, new)
            if k:
                new = (bl if k == "BL" else __import__("thumb").b_w)(addr, sa)
            else:
                new = struct.pack("<I", sa | 1)
            out.append((sa, b"\xff" * SLOT, sb))
        out.append((addr, old, new))
    return out
