#!/usr/bin/env python3
"""Versions of the patcher and of each machine, and the list of what changed (docs/versions.json, shown on the page).

  python3 tools/versions.py show
  python3 tools/versions.py check                      fails if a machine's (or the patcher's) code changed since its version was set
  python3 tools/versions.py bump <component> "what changed"     1.0 -> 1.1 (add --major for 2.0), records the code hash, adds the line to the list

Components: patcher, looper, samplr, trig, drone. MCL has no version here (its own project versions it).
A machine's version covers ITS files (below). The patcher's covers the page, the shared code (the Looper page host `looper_page.c`, which draws every page,
`solo.c`), the 3.1.n features, the build tools and everything else that is not a machine's or MCL's. Bump a machine when its files changed; bump the patcher
when anything shared changed (the page text, a fix to the filter, a change to the page host ...). Several components can change at once: bump each.
"""
import hashlib
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILE = os.path.join(ROOT, "docs", "versions.json")

OWNED = {
    "looper": ["src/looper.c", "src/looper.h", "src/looper_thunk.S", "patches/looper.py"],
    "samplr": ["src/samplr.c", "src/samplr.h"],
    "trig": ["src/t2m.c", "src/t2m.h", "src/t2m_bb.c", "src/t2m_bb.h", "src/t2m_page.c"],
    "drone": ["src/lira.c", "src/lira.h", "src/lira_pad.c", "src/lira_chord.c", "src/lira_chord.h", "src/lira_ui.h", "src/lira_ui.inc", "src/lira_thunk.S",
              "src/lira/", "patches/lira.py", "patches/lirarec.py"],
}
MCL = ("src/mcl_bb/", "tools/mcl/", "patches/mcl", "test_mcl", "test_mclpad.py")      # not versioned here
PATCHER_ROOTS = ["src/", "patches/", "docs/index.html", "docs/apply.js", "docs/manual.json", "tools/manual_build.py", "build.sh", "tools/patchsets.py", "tools/combo.py", "tools/export_web.py"]
IGNORE = ("__pycache__", ".pyc")


def tracked():
    out = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True).stdout.split("\n")
    return [f for f in out if f and not any(i in f for i in IGNORE)]


def files_of(component):
    allf = tracked()
    owned = {c: [f for f in allf if any(f == p or (p.endswith("/") and f.startswith(p)) for p in ps)] for c, ps in OWNED.items()}
    if component != "patcher":
        return sorted(owned[component])
    taken = {f for fs in owned.values() for f in fs}
    return sorted(f for f in allf if any(f == r or (r.endswith("/") and f.startswith(r)) for r in PATCHER_ROOTS) and f not in taken and not f.startswith(MCL))


def code_hash(component):
    h = hashlib.sha256()
    for f in files_of(component):
        h.update(f.encode() + b"\0" + open(os.path.join(ROOT, f), "rb").read() + b"\0")
    return h.hexdigest()[:16]


def load():
    return json.load(open(FILE))


def save(d):
    json.dump(d, open(FILE, "w"), indent=1, ensure_ascii=False)
    open(FILE, "a").write("\n")


def comp(d, c):
    return d if c == "patcher" else d["machines"][c]


def cmd_show(d):
    print(f"{d['name']} {d['version']}")
    for c in OWNED:
        print(f"  {d['machines'][c]['title']:14s} {d['machines'][c]['version']}")
    print(f"{len(d['changes'])} entries in the list")


def cmd_check(d):
    bad = []
    for c in ["patcher"] + list(OWNED):
        cur, rec = code_hash(c), comp(d, c).get("code_hash")
        if cur != rec:
            bad.append(c)
    if bad:
        print("changed since their version was set (bump them: python3 tools/versions.py bump <component> \"what changed\"):", ", ".join(bad))
        return 1
    print("versions match the code")
    return 0


def cmd_bump(d, c, text, major):
    if c not in ["patcher"] + list(OWNED):
        raise SystemExit("component: patcher " + " ".join(OWNED))
    e = comp(d, c)
    a, b = (int(x) for x in e["version"].split("."))
    e["version"] = f"{a + 1}.0" if major else f"{a}.{b + 1}"
    e["code_hash"] = code_hash(c)
    d["changes"].insert(0, {"date": subprocess.run(["date", "+%Y-%m-%d"], capture_output=True, text=True).stdout.strip(), "component": c, "version": e["version"], "text": text})
    save(d)
    print(c, "->", e["version"])


if __name__ == "__main__":
    a = sys.argv[1:]
    d = load()
    if not a or a[0] == "show":
        cmd_show(d)
    elif a[0] == "check":
        sys.exit(cmd_check(d))
    elif a[0] == "bump" and len(a) >= 3:
        cmd_bump(d, a[1], a[2], "--major" in a)
    else:
        print(__doc__)
