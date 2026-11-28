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
import b2guard  # noqa: E402

BASE = 0x08040000
stock = open(os.path.join(ROOT, "firmware", "BLACKBOX-3.1.9.bin"), "rb").read()
sys.argv = ["patch.py"]                                  # cave.py reads the set list: none here (the page sets the flag bytes itself)

def enc(a, new):
    return {"o": a - BASE, "b64": base64.b64encode(new).decode()} if len(new) > 4096 else {"o": a - BASE, "hex": new.hex()}


sets = {}
have_mcl = os.path.exists(os.path.join(ROOT, "out", "mcl.elf"))
for s in PS.CANON:
    if s == "cave" or (PS.SETS[s].get("mcl") and not have_mcl):
        continue
    if s == "mclsd":
        sys.argv = ["patch.py", "lirasave"]                  # the form that runs with `lirasave` (every part has it): its pointers are cave variables
        importlib.invalidate_caches()
    sets[s] = [enc(a, new) for a, old, new in b2guard.transform(importlib.import_module(s).PATCHES)]      # (hooks into bank 2 point at their guard stubs; the stubs are patches too)
sys.argv = ["patch.py"]

cave = importlib.import_module("cave")
ver = [p for p in cave.PATCHES if p[1] and p[0] < cave.STOCK_END]            # the version string
app = [p for p in cave.PATCHES if not p[1] and p[0] == cave.STOCK_END]       # appended: the core (flash bank 1)
assert len(ver) == 1 and len(app) == 1 and app[0][0] == cave.STOCK_END == BASE + len(stock)
code = app[0][2]
sym = cave.symbols(os.path.join(ROOT, "out", "cave.elf"))
# the payload of flash bank 2 (the machines): cave.py makes it when a set of cave.B2_SETS is in the list; its header and the sum bank 1 expects are finished by apply.js
sys.argv = ["patch.py", "looper"]
import importlib as _il
cave = _il.reload(cave)
pay = [p for p in cave.PATCHES if p[0] == cave.B2_BASE]
assert len(pay) == 1 and cave.B2_LAYOUT
L = cave.B2_LAYOUT
sys.argv = ["patch.py"]
data_cave = {
    "version": {"o": ver[0][0] - BASE, "hex": ver[0][2].hex()},
    "code_o": cave.STOCK_END - BASE, "code": base64.b64encode(code).decode(),
    "host_o": sym["bb_host"] - cave.STOCK_END,                                  # offsets inside `code` of the page flags (patches/cave.py)
    "hide_o": sym["bb_hide"] - cave.STOCK_END,
    "b2_sets": list(cave.B2_SETS),                                              # a set list with one of these has a payload (BBEXTRA.BIN)
    "b2_o": cave.B2_BASE - BASE, "b2": base64.b64encode(pay[0][2]).decode(),    # the payload, header still 0xFF
    "b2_itcm_off": L["itcm_off"], "b2_itcm_len": L["itcm_len"], "b2_itcm_dst": L["itcm_dst"],
    "sum_o": sym["b2_expect_sum"] - BASE,                                       # where bank 1 keeps the sum of the payload it expects
}
reg = json.load(open(os.path.join(ROOT, "docs", "combos.json")))
tested = {k: {"status": e["status"], "scope": e.get("scope", "full"), "tests": len(e.get("tests", {})), "date": e["tested_at"], "rev": e["source_rev"]}
          for k, e in reg["combos"].items() if e["status"] == "pass"}
out = {
    "name": "blackbox-mod", "stock_version": "3.1.9", "stock_size": len(stock), "stock_sha256": hashlib.sha256(stock).hexdigest(),
    "canon": [s for s in PS.CANON if s in sets or s == "cave"],
    "requires": {s: PS.SETS[s]["requires"] for s in PS.CANON if s in sets or s == "cave"},
    "core": PS.CORE,
    "parts": {k: {"title": d["title"], "sets": d["sets"], "hide": d["hide"], "available": True} for k, d in PS.FEATURES.items() if k != "mcl" or have_mcl},
    "addons": {k: {"title": d["title"], "sets": d["sets"], "group": d.get("group", "addons")} for k, d in PS.ADDONS.items()},
    "always": ["looper", "lirasave"],
    "sets": sets, "cave": data_cave,
    "cave_bytes": len(code), "cave_limit": PS.CAVE_LIMIT,
}
os.makedirs(os.path.join(ROOT, "docs"), exist_ok=True)
path = os.path.join(ROOT, "docs", "data.json")
json.dump(out, open(path, "w"), separators=(",", ":"))
print(f"wrote {path}: {os.path.getsize(path):,} bytes, {len(sets)} sets, {sum(len(v) for v in sets.values())} hooks, bank 1 cave {len(code):,}, bank 2 payload {len(pay[0][2]):,}, {len(tested)} tested combinations")
