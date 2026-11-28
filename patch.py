"""Apply byte patches to the stock Blackbox firmware and write the SD-card-ready files.

  python3 patch.py <patchset> [...]     e.g. python3 patch.py version_tag

Each patchset in patches/ is a module defining PATCHES = [(addr, old_bytes, new_bytes), ...].
Every patch verifies the stock bytes before writing, so a patch built for the wrong firmware fails loudly.

Output (out/<sets>/): BLACKBOX.bin = the stock image plus the core (flash bank 1, it ends before 0x08100000: the stock installer of some units writes bank 1 only),
BBEXTRA.BIN = the payload for flash bank 2 when the sets need one (src/b2.h; the unit programs it into bank 2 itself, from the card), and FULL.bin = both laid
out as the unit sees them (bank 2 at 0x08100000), which is what the emulator tests load.
"""
import hashlib
import importlib
import os
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
STOCK = os.path.join(ROOT, "firmware", "BLACKBOX-3.1.9.bin")
STOCK_SHA256 = "281ae303d32e5eb52adca7817a648a34e4bd3848017f26673dd9df3905f1341d"
BASE = 0x08040000
B2_BASE = 0x08100000

data = bytearray(open(STOCK, "rb").read())
assert hashlib.sha256(data).hexdigest() == STOCK_SHA256, "stock firmware hash mismatch"

sys.path.insert(0, os.path.join(ROOT, "patches"))
import b2guard  # noqa: E402

names = sys.argv[1:]
order = (["cave"] if "cave" in names else []) + [n for n in names if n != "cave"]     # the cave first: other sets patch into it (guard stubs, the MCL pointers)
payload = None
for name in order:
    for addr, old, new in b2guard.transform(importlib.import_module(name).PATCHES):
        if addr >= B2_BASE:                                  # the payload for bank 2
            o = addr - B2_BASE
            if payload is None:
                assert addr == B2_BASE and old == b"", f"{name}: patch at {addr:#x} before the payload exists"
                payload = bytearray(new)
                continue
            assert payload[o:o + len(old)] == old, f"{name}: payload bytes differ at {addr:#x}"
            payload[o:o + len(new)] = new
            continue
        o = addr - BASE
        if o == len(data):
            assert old == b"", f"{name}: append at {addr:#x} must have empty old bytes"
            data += new
            continue
        assert data[o:o + len(old)] == old, f"{name}: stock bytes differ at {addr:#x}"
        assert len(old) == len(new), f"{name}: size change at {addr:#x}"
        data[o:o + len(new)] = new
        print(f"{name}: {addr:#010x} {old.hex()} -> {new.hex()}")

if payload is not None:                                      # the header, and the sum bank 1 expects of the payload (src/b2.c)
    import cave
    from thumb import symbols
    hdr, sm = cave.b2_header(payload)
    payload[:32] = hdr
    at = symbols(os.path.join(ROOT, "out", "cave.elf"))["b2_expect_sum"] - BASE
    assert data[at:at + 4] == b"\xff\xff\xff\xff"
    data[at:at + 4] = sm.to_bytes(4, "little")
assert BASE + len(data) <= B2_BASE, f"BLACKBOX.bin runs past flash bank 1 ({BASE + len(data):#x})"

out_dir = os.path.join(ROOT, "out", "+".join(names))
os.makedirs(out_dir, exist_ok=True)
out = os.path.join(out_dir, "BLACKBOX.bin")
open(out, "wb").write(data)
if payload is not None:
    open(os.path.join(out_dir, "BBEXTRA.BIN"), "wb").write(payload)
    open(os.path.join(out_dir, "FULL.bin"), "wb").write(bytes(data).ljust(B2_BASE - BASE, b"\xff") + payload)
else:
    for f in ("BBEXTRA.BIN", "FULL.bin"):
        if os.path.exists(os.path.join(out_dir, f)):
            os.remove(os.path.join(out_dir, f))
    open(os.path.join(out_dir, "FULL.bin"), "wb").write(bytes(data))
print(f"wrote {out} ({len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()[:16]})" + (f" + BBEXTRA.BIN ({len(payload)} bytes)" if payload is not None else ""))
