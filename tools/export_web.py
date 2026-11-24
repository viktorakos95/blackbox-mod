#!/usr/bin/env python3
"""Export the patch sets for the web patcher: docs/data.json.

Only the bytes this project adds or changes are exported (hooks, the compiled code, the ITCM image); no stock bytes. The page checks the user's file by SHA-256
(the stock image's) and applies the chosen sets with docs/apply.js; tools/verify_web.js checks that its output is byte-identical to patch.py's.

  python3 tools/export_web.py          (needs `sh build.sh` first; MCL sets are not exported: the blob is not built here)
"""
import base64
import hashlib
import importlib
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "patches"))
import patchsets as PS  # noqa: E402

BASE = 0x08040000
stock = open(os.path.join(ROOT, "firmware", "BLACKBOX-3.1.9.bin"), "rb").read()
sys.argv = ["patch.py"]                                  # cave.py reads the set list: none here (the page sets the flag bytes itself)

sets = {}
for s in PS.CANON:
    if PS.SETS[s].get("mcl") or s == "cave":
        continue
    sets[s] = [{"o": a - BASE, "hex": new.hex()} for a, old, new in importlib.import_module(s).PATCHES]

cave = importlib.import_module("cave")
ver = [p for p in cave.PATCHES if p[1] and p[0] < cave.STOCK_END]            # the version string
app = [p for p in cave.PATCHES if not p[1]]                                    # appended: the code, then 0xFF fill + the ITCM image
assert len(ver) == 1 and len(app) == 2 and app[0][0] == cave.STOCK_END == BASE + len(stock)
code = app[0][2]
tail = app[1][2]
itcm_at = cave.ITCM_LOAD
fill = itcm_at - (cave.STOCK_END + len(code))
assert tail[:fill] == b"\xff" * fill
sym = cave.symbols(os.path.join(ROOT, "out", "cave.elf")) if hasattr(cave, "symbols") else None
data_cave = {
    "version": {"o": ver[0][0] - BASE, "hex": ver[0][2].hex()},
    "code_o": cave.STOCK_END - BASE, "code": base64.b64encode(code).decode(),
    "fill_o": cave.STOCK_END - BASE + len(code), "fill_len": fill,
    "itcm_o": itcm_at - BASE, "itcm": base64.b64encode(tail[fill:]).decode(),
    "host_o": sym["bb_host"] - cave.STOCK_END,                                  # offsets inside `code` of the page flags (patches/cave.py)
    "hide_o": sym["bb_hide"] - cave.STOCK_END,
}
reg = json.load(open(os.path.join(ROOT, "docs", "combos.json")))
tested = {k: {"status": e["status"], "scope": e.get("scope", "full"), "tests": len(e.get("tests", {})), "date": e["tested_at"], "rev": e["source_rev"]}
          for k, e in reg["combos"].items() if e["status"] == "pass"}
out = {
    "name": "blackbox-mod", "stock_version": "3.1.9", "stock_size": len(stock), "stock_sha256": hashlib.sha256(stock).hexdigest(),
    "canon": [s for s in PS.CANON if s in sets or s == "cave"],
    "requires": {s: PS.SETS[s]["requires"] for s in PS.CANON if s in sets or s == "cave"},
    "core": PS.CORE,
    "parts": {k: {"title": d["title"], "sets": d["sets"], "hide": d["hide"], "available": True} for k, d in PS.FEATURES.items() if k != "mcl"},   # (MCL is not offered)
    "addons": {k: {"title": d["title"], "sets": d["sets"], "group": d.get("group", "addons")} for k, d in PS.ADDONS.items()},
    "always": ["looper", "lirasave"],
    "sets": sets, "cave": data_cave,
    "cave_bytes": len(code), "cave_limit": PS.CAVE_LIMIT,
}
os.makedirs(os.path.join(ROOT, "docs"), exist_ok=True)
path = os.path.join(ROOT, "docs", "data.json")
json.dump(out, open(path, "w"), separators=(",", ":"))
print(f"wrote {path}: {os.path.getsize(path):,} bytes, {len(sets)} sets, {sum(len(v) for v in sets.values())} hooks, cave {len(code):,}, itcm {len(tail) - fill:,}, {len(tested)} tested combinations")
