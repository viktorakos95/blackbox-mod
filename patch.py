"""Apply byte patches to the stock Blackbox firmware and write an SD-card-ready BLACKBOX.bin.

  python3 patch.py <patchset> [...]     e.g. python3 patch.py version_tag

Each patchset in patches/ is a module defining PATCHES = [(addr, old_bytes, new_bytes), ...].
Every patch verifies the stock bytes before writing, so a patch built for the wrong firmware fails loudly.
"""
import hashlib
import importlib
import os
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
STOCK = os.path.join(ROOT, "firmware", "BLACKBOX-3.1.9.bin")
STOCK_SHA256 = "281ae303d32e5eb52adca7817a648a34e4bd3848017f26673dd9df3905f1341d"
BASE = 0x08040000

data = bytearray(open(STOCK, "rb").read())
assert hashlib.sha256(data).hexdigest() == STOCK_SHA256, "stock firmware hash mismatch"

sys.path.insert(0, os.path.join(ROOT, "patches"))
names = sys.argv[1:]
for name in names:
    for addr, old, new in importlib.import_module(name).PATCHES:
        o = addr - BASE
        if o == len(data):
            assert old == b"", f"{name}: append at {addr:#x} must have empty old bytes"
            data += new
            continue
        assert data[o:o + len(old)] == old, f"{name}: stock bytes differ at {addr:#x}"
        assert len(old) == len(new), f"{name}: size change at {addr:#x}"
        data[o:o + len(new)] = new
        print(f"{name}: {addr:#010x} {old.hex()} -> {new.hex()}")

out_dir = os.path.join(ROOT, "out", "+".join(names))
os.makedirs(out_dir, exist_ok=True)
out = os.path.join(out_dir, "BLACKBOX.bin")
open(out, "wb").write(data)
print(f"wrote {out} ({len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()[:16]})")
