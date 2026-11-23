#!/usr/bin/env python3
"""The web patcher does what patch.py does: for every combination of parts (MCL aside) and add-ons, patch.py builds the image and docs/apply.js (run by node)
builds it from docs/data.json; the set lists and the SHA-256 must match. `python3 tools/verify_web.py [--quick]` (about 2 s a combination)."""
import itertools
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import combo as C  # noqa: E402
import patchsets as PS  # noqa: E402

parts_all = [p for p in PS.FEATURES if p != "mcl"]
addons_all = list(PS.ADDONS)
cases = []
for r in range(len(parts_all) + 1):
    for parts in itertools.combinations(parts_all, r):
        for k in range(len(addons_all) + 1):
            for addons in itertools.combinations(addons_all, k):
                if "clkmon" in addons and not parts:
                    continue                                   # (the page disables it: the monitor needs a machine)
                cases.append((list(parts), list(addons)))
if "--quick" in sys.argv:
    cases = cases[::9]
out = []
for parts, addons in cases:
    sets = C.from_parts(parts, addons)
    assert not C.problems(sets), (parts, addons, C.problems(sets))
    r = subprocess.run([sys.executable, "patch.py"] + sets, cwd=ROOT, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-400:]
    import hashlib
    d = os.path.join(ROOT, "out", "+".join(sets))
    out.append(dict(parts=parts, addons=addons, sets=sets, sha=hashlib.sha256(open(os.path.join(d, "BLACKBOX.bin"), "rb").read()).hexdigest()))
    shutil.rmtree(d)
os.makedirs(os.path.join(ROOT, "out"), exist_ok=True)
json.dump(out, open(os.path.join(ROOT, "out", "web_cases.json"), "w"))
print(f"{len(out)} combinations built with patch.py; running the page's logic ...", flush=True)
sys.exit(subprocess.run(["node", os.path.join(ROOT, "tools", "verify_web.js"), os.path.join(ROOT, "out", "web_cases.json")]).returncode)
