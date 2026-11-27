#!/usr/bin/env python3
"""Build a combination of patch sets, run the emulator tests that apply to it, and report pass / fail.

  python3 tools/combo.py list                         sets, presets, tests (and which images each test names)
  python3 tools/combo.py resolve looper lira          add what is required, put in order
  python3 tools/combo.py build  drone                 -> out/combos/<id>/BLACKBOX.bin   (a preset name, or set names)
  python3 tools/combo.py test   drone [-j 4] [--only test_looper.py] [--record]
  python3 tools/combo.py host   [-j 4] [--record]     the tests that need no image (they test the C source on the host)
  python3 tools/combo.py table                        regenerate docs/patch-sets.md
  python3 tools/combo.py verify-order [-n 7]          the sets' order does not change the image
  python3 tools/combo.py verify <BLACKBOX.bin> <sets>  an image (e.g. the web patcher's) holds exactly the hooks of those sets
  python3 tools/combo.py control                       which tests pass on an image with no hooks (docs/test-sensitivity.json)

How a test is run against a combination: the tests name the exact image they want (`out/solo+slice+...+looper+cave/BLACKBOX.bin`). A test applies to a combination
when every image it names is made of sets the combination has. It then runs in a private directory (out/combos/<id>/sb, the repository's files linked in, its own
out/) where each of those names holds the COMBINATION's image, so the test checks the real thing, extras included, and the repository's out/ is never given a
misnamed image. A test that names an image the combination lacks is "n/a" (not run), never a pass.

`--record` writes the result into docs/combos.json, the list of combinations the web patcher may offer (status "pass" only when every applicable test passed).

The MCL program: out/mcl.bin / out/mcl.elf must be the REAL build (~402,000 bytes for MCL 5.10+; tools/mcl/build_blob.sh / build_mcl.sh). test_mcl_sd.py rebuilds
them as a 19 KB test build, so this tool snapshots the real one first (out/mcl-real/), refuses any image build while out/mcl.bin is not in range, and restores
the snapshot after every run that touched it. Without the blob, MCL combinations are reported "untested (no MCL blob)" and never recorded as pass.
"""
import argparse
import concurrent.futures as cf
import datetime
import hashlib
import json
import os
import random
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import patchsets as PS  # noqa: E402

OUT = os.path.join(ROOT, "out")
REGISTRY = os.path.join(ROOT, "docs", "combos.json")


# ---------------------------------------------------------------- sets, presets, order
def expand(names):
    out = []
    for n in names:
        if n in PS.SETS:                       # (a set wins: a preset must never share a set's name)
            out.append(n)
        elif n in PS.PRESETS:
            out += PS.PRESETS[n]["sets"]
        else:
            raise SystemExit(f"unknown set or preset: {n} (sets: {' '.join(PS.CANON)}; presets: {' '.join(PS.PRESETS)})")
    return out


def parse_parts(names):
    """`parts=samplr,trig addons=tools,save` -> (parts, addons) or None when the names are plain sets / presets."""
    parts = addons = None
    for n in names:
        if n.startswith("parts="):
            parts = [x for x in n[6:].split(",") if x]
        elif n.startswith("addons="):
            addons = [x for x in n[7:].split(",") if x]
    if parts is None and addons is None:
        return None
    for x in parts or []:
        if x not in PS.FEATURES:
            raise SystemExit(f"unknown part {x} (parts: {' '.join(PS.FEATURES)})")
    for x in addons or []:
        if x not in PS.ADDONS:
            raise SystemExit(f"unknown add-on {x} (add-ons: {' '.join(PS.ADDONS)})")
    return parts or [], addons or []


def resolve(names):
    """Names (sets / presets / parts= addons=) -> the sets with everything they require, plus cave, in the canonical order."""
    pa = parse_parts(names)
    if pa:
        return from_parts(*pa)
    todo, seen = list(expand(names)) + ["cave"], set()
    while todo:
        s = todo.pop()
        if s in seen:
            continue
        seen.add(s)
        todo += PS.SETS[s]["requires"]
    return [s for s in PS.CANON if s in seen]


def problems(sets):
    """Unmet requirements of an explicit list (what the web page greys out)."""
    have = set(sets)
    out = [f"{s} needs {r}" for s in sets for r in PS.SETS[s]["requires"] if r not in have]
    if "looper" in have:                                    # the host: the pages it shows must make sense
        if {"hide_looper", "hide_samplr", "hide_trig", "hide_drone"} <= have and "mclblob" not in have:
            out.append("`looper` with every page hidden: there is nothing to show")
        if "hide_drone" not in have and "lira" not in have:
            out.append("the DRONE-8 page is not hidden but `lira` (its engine) is not in the list")
    return out


def from_parts(parts, addons=()):
    """The sets of a free combination: parts = some of looper samplr trig drone mcl, addons = some of tools rec clkmon crash. Returns the sets in order."""
    parts, sets = set(parts), list(PS.CORE)
    for p in parts:
        sets += PS.FEATURES[p]["sets"]
    if parts & {"looper", "samplr", "trig", "drone", "mcl"}:
        sets += ["looper", "lirasave"]                      # saving comes with every part
        sets += [PS.FEATURES[p]["hide"] for p in ("looper", "samplr", "trig", "drone") if p not in parts]
    for a in addons:
        sets += PS.ADDONS[a]["sets"]
    return resolve(sets)


def combo_id(sets, names=None):
    pa = parse_parts(names or [])
    if pa:
        return "p-" + "-".join(pa[0]) + ("_a-" + "-".join(pa[1]) if pa[1] else "")
    if names and len(names) == 1 and names[0] in PS.PRESETS and resolve(names) == resolve(PS.PRESETS[names[0]]["sets"]):
        return names[0]
    return "+".join(sets)


def needs_mcl(sets):
    return any(PS.SETS[s].get("mcl") for s in sets)


# ---------------------------------------------------------------- the build
def sh(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **kw)


def ensure_cave():
    """out/cave.elf / cave.bin / itcm.bin from src/ (build.sh), unless they are newer than every source."""
    elf = os.path.join(OUT, "cave.elf")
    srcs = [os.path.join(dp, f) for dp, _, fs in os.walk(os.path.join(ROOT, "src")) for f in fs if not dp.startswith(os.path.join(ROOT, "src", "mcl_bb"))]
    srcs.append(os.path.join(ROOT, "build.sh"))
    if not os.path.exists(elf) or os.path.getmtime(elf) < max(os.path.getmtime(s) for s in srcs):
        print("building the cave (build.sh) ...", flush=True)
        r = sh(["sh", "build.sh"])
        if r.returncode:
            raise SystemExit("build.sh failed:\n" + r.stdout[-1500:] + r.stderr[-3000:])
    cave = open(os.path.join(OUT, "cave.bin"), "rb").read()
    if len(cave) > PS.CAVE_LIMIT:
        raise SystemExit(f"the cave is {len(cave)} bytes, past the limit {PS.CAVE_LIMIT}")
    return len(cave), hashlib.sha256(cave).hexdigest()


def blob_ok():
    """(ok, why). The real MCL program only: the SD test build is ~19 KB."""
    b, e = os.path.join(OUT, "mcl.bin"), os.path.join(OUT, "mcl.elf")
    if not (os.path.exists(b) and os.path.exists(e)):
        return False, "no out/mcl.bin / out/mcl.elf (build the blob: MCL_DIR=... MCL_GFX=... tools/mcl/build_mcl.sh)"
    n = os.path.getsize(b)
    lo, hi = PS.MCL_BLOB_RANGE
    if not lo <= n <= hi:
        return False, f"out/mcl.bin is {n} bytes, not the real program ({lo}..{hi}): a test build? rebuild it before making any image"
    return True, f"{n} bytes"


SNAP = os.path.join(OUT, "mcl-real")


def blob_snapshot():
    ok, _ = blob_ok()
    if ok:
        os.makedirs(SNAP, exist_ok=True)
        for f in ("mcl.bin", "mcl.elf"):
            shutil.copy2(os.path.join(OUT, f), os.path.join(SNAP, f))


def blob_restore_if_changed():
    """After a run: if the tests replaced out/mcl.* (test_mcl_sd.py does), put the real one back. Returns a note or None."""
    if not os.path.exists(os.path.join(SNAP, "mcl.bin")):
        return None
    note = None
    for f in ("mcl.bin", "mcl.elf"):
        a, b = os.path.join(OUT, f), os.path.join(SNAP, f)
        if not os.path.exists(a) or open(a, "rb").read() != open(b, "rb").read():
            shutil.copy2(b, a)
            note = "out/mcl.* had been replaced by a test build: the real one is restored"
    return note


def build(sets):
    """patch.py with the sets in order; returns (path of the image, sha256). The only patch logic there is."""
    if needs_mcl(sets):
        ok, why = blob_ok()
        if not ok:
            raise SystemExit("MCL combination, " + why)
    r = sh([sys.executable, "patch.py"] + sets)
    if r.returncode:
        raise SystemExit("patch.py failed:\n" + (r.stderr or r.stdout)[-2000:])
    src = os.path.join(OUT, "+".join(sets), "BLACKBOX.bin")
    data = open(src, "rb").read()
    return src, hashlib.sha256(data).hexdigest(), len(data)


# ---------------------------------------------------------------- the tests
IMG_RE = re.compile(r"out/([A-Za-z0-9_+\-]+)/BLACKBOX\.bin")


def test_files():
    return sorted(f for f in os.listdir(ROOT) if re.fullmatch(r"test_.*\.py", f))


def test_info(f):
    """kind: 'mcl' (needs the MCL checkout / blob), 'image' (names the image(s) it runs on), 'host' (no image: tests the C source)."""
    src = open(os.path.join(ROOT, f)).read()
    if f.startswith("test_mcl"):
        return dict(kind="mcl", images=[])
    # a test may run another test's source (test_samplr.py execs the top of test_looper_v4.py): its image is that one's
    for other in re.findall(r'open\("(test_\w+\.py)"\)', src):
        if other != f and os.path.exists(os.path.join(ROOT, other)):
            src += open(os.path.join(ROOT, other)).read()
    imgs = sorted({tuple(n.split("+")) for n in IMG_RE.findall(src)})
    return dict(kind="image" if imgs else "host", images=[list(i) for i in imgs])


def visible_pages(sets):
    """The pages (looper samplr trig drone) a list of sets leaves in the image."""
    have = set(sets)
    if "looper" not in have:
        return set()
    return {p for p in ("looper", "samplr", "trig", "drone") if "hide_" + p not in have and (p != "drone" or "lira" in have)}


def applicable(info, sets, f=None):
    have = set(sets)
    if f in PS.TEST_PAGES and not set(PS.TEST_PAGES[f]) <= visible_pages(sets):
        return False
    return info["kind"] == "image" and all(set(i) <= have for i in info["images"])


def make_sandbox(cid, image, sets, test):
    """A private directory for ONE test: the repository's files linked in, its own out/ with the cave files linked and the image COPIED under every name the test
    uses. Copies, never links: a test may write an image itself (test_lira_hooks runs patch.py), and a write through a link would change the image of the
    combination under test (and of the tests running beside it)."""
    sb = os.path.join(OUT, "combos", cid, "sb", test)
    shutil.rmtree(sb, ignore_errors=True)
    os.makedirs(os.path.join(sb, "out"))
    for e in os.listdir(ROOT):
        if e not in ("out", ".git"):
            os.symlink(os.path.join(ROOT, e), os.path.join(sb, e))
    for f in ("cave.bin", "cave.elf", "itcm.bin", "munchi.o"):
        if os.path.exists(os.path.join(OUT, f)):
            shutil.copy2(os.path.join(OUT, f), os.path.join(sb, "out", f))
    if needs_mcl(sets):
        for f in ("mcl.bin", "mcl.elf"):
            shutil.copy2(os.path.join(OUT, f), os.path.join(sb, "out", f))
    for im in test_info(test)["images"]:
        n = "+".join(im)
        os.makedirs(os.path.join(sb, "out", n), exist_ok=True)
        shutil.copyfile(image, os.path.join(sb, "out", n, "BLACKBOX.bin"))
    return sb


def run_test(f, cid, image, sets, logdir, timeout=1800):
    sb = make_sandbox(cid, image, sets, f)
    t0 = time.time()
    try:
        r = subprocess.run([sys.executable, f], cwd=sb, capture_output=True, text=True, timeout=timeout)
        out, rc = r.stdout + r.stderr, r.returncode
    except subprocess.TimeoutExpired as ex:
        out, rc = (ex.stdout or b"").decode(errors="replace") + "\nTIMEOUT", -9
    os.makedirs(logdir, exist_ok=True)
    open(os.path.join(logdir, f + ".log"), "w").write(out)
    fails = [l for l in out.splitlines() if l.startswith("FAIL")]
    st = "pass" if rc == 0 and not fails else "FAIL"
    if st == "pass":
        shutil.rmtree(sb, ignore_errors=True)          # (kept on a failure, to look at)
    return f, st, round(time.time() - t0), fails[:3]


def run_all(files, cid, image, sets, logdir, jobs):
    res = {}
    # the long tests first so the pool is not left waiting on one
    files = sorted(files, key=lambda f: -os.path.getsize(os.path.join(ROOT, f)))
    with cf.ThreadPoolExecutor(jobs) as ex:
        for f, st, secs, fails in ex.map(lambda f: run_test(f, cid, image, sets, logdir), files):
            res[f] = dict(status=st, seconds=secs, fails=fails)
            print(f"  {st:4s} {f} ({secs}s)" + ("  " + fails[0][:120] if fails else ""), flush=True)
    return res


def wiring_check(image, sets):
    """Independent of patch.py's own writing: the image holds every hook of each chosen set, and the sites of each set NOT chosen are still stock.
    (The web patcher's output goes through the same function: `combo.py verify <image> <sets>`.) Returns a list of faults."""
    import importlib
    sys.path.insert(0, os.path.join(ROOT, "patches"))
    old_argv = sys.argv
    sys.argv = ["patch.py"] + list(sets)            # cave.py and mclsd.py read the set list (bb_host / bb_hide, the lirasave form)
    base, faults = 0x08040000, []
    stock = open(os.path.join(ROOT, "firmware", "BLACKBOX-3.1.9.bin"), "rb").read()
    have = set(sets)
    for s in PS.CANON:
        if PS.SETS[s].get("mcl") and not blob_ok()[0]:
            if s in have:
                faults.append(f"{s}: not checked (no real MCL blob here)")
            continue
        mod = importlib.import_module(s)
        if s == "cave":
            mod = importlib.reload(mod)
        for addr, old, new in mod.PATCHES:
            o = addr - base
            if s in have:
                exp = bytearray(new)
                if s == "cave":                                   # the MCL sets write into the cave's own bytes (its entry pointer, the card-task pointers): expect their bytes there
                    for m in ("mclblob", "mclsd"):
                        if m in have and not (PS.SETS[m].get("mcl") and not blob_ok()[0]):
                            for a2, _o2, n2 in importlib.import_module(m).PATCHES:
                                lo, hi = max(addr, a2), min(addr + len(new), a2 + len(n2))
                                if lo < hi:
                                    exp[lo - addr:hi - addr] = n2[lo - a2:hi - a2]
                if image[o:o + len(exp)] != bytes(exp):
                    faults.append(f"{s}: hook at {addr:#x} missing")
            elif old:
                if o < len(stock) and image[o:o + len(old)] != stock[o:o + len(old)]:
                    faults.append(f"{s} is not in the combination but {addr:#x} is not stock")
    sys.argv = old_argv
    return faults


def cmd_verify(a):
    faults = wiring_check(open(a.image, "rb").read(), resolve(a.names))
    print("\n".join(faults) or "wiring OK")
    return 1 if faults else 0


def cmd_control(a):
    """Run every image test against an image with NO hooks (stock + the code block). A test that still passes does not look at the hooks."""
    ensure_cave()
    r = sh([sys.executable, "patch.py", "cave"])
    assert r.returncode == 0, r.stderr
    tests = [f for f in test_files() if test_info(f)["kind"] == "image"]
    res = run_all(tests, "_control", os.path.join(OUT, "cave", "BLACKBOX.bin"), ["cave"], os.path.join(OUT, "combos", "_control", "logs"), a.jobs)
    ins = sorted(f for f, v in res.items() if v["status"] == "pass")
    json.dump(dict(note="Image tests that PASS on an image with no hooks (tools/combo.py control): they test the code in the cave, not that a combination's hooks reach it.",
                   hook_blind=ins, hook_sensitive=sorted(set(res) - set(ins)), tested_at=datetime.date.today().isoformat(), source_rev=git_rev()),
              open(os.path.join(ROOT, "docs", "test-sensitivity.json"), "w"), indent=1)
    print(f"hook-blind: {len(ins)} of {len(tests)}")
    return 0


def git_rev():
    r = sh(["git", "rev-parse", "--short", "HEAD"])
    dirty = sh(["git", "status", "--porcelain", "--", "src", "patches", "build.sh", "emu.py", "tools"]).stdout.strip()
    return (r.stdout.strip() or "?") + ("+dirty" if dirty else "")


def record(entry):
    reg = json.load(open(REGISTRY)) if os.path.exists(REGISTRY) else dict(note="Written by tools/combo.py --record. The web patcher offers only status 'pass'.", combos={})
    reg["combos"][entry["id"]] = entry
    json.dump(reg, open(REGISTRY, "w"), indent=1, sort_keys=True)
    open(REGISTRY, "a").write("\n")


# ---------------------------------------------------------------- commands
def cmd_test(a):
    sets = resolve(a.names)
    cid = combo_id(sets, a.names)
    bad = problems(sets)
    assert not bad, bad
    cave_n, cave_sha = ensure_cave()
    blob_snapshot()
    entry = dict(id=cid, sets=sets, title=PS.PRESETS.get(cid, {}).get("title", ""), experimental=bool(PS.PRESETS.get(cid, {}).get("experimental")),
                 tested_at=datetime.date.today().isoformat(), source_rev=git_rev(), cave_bytes=cave_n, cave_limit=PS.CAVE_LIMIT, cave_sha256=cave_sha)
    pa = parse_parts(a.names)
    if pa:
        entry.update(parts=pa[0], addons=pa[1], title="parts: " + (", ".join(pa[0]) or "-") + ("; add-ons: " + ", ".join(pa[1]) if pa[1] else ""))
    if needs_mcl(sets):
        ok, why = blob_ok()
        if not ok:
            entry.update(status="untested", reason="no real MCL blob here: " + why, tests={})
            print(f"{cid}: UNTESTED, {entry['reason']}")
            if a.record:
                record(entry)
            return 2
    image, sha, size = build(sets)
    dst = os.path.join(OUT, "combos", cid)
    os.makedirs(dst, exist_ok=True)
    shutil.copy2(image, os.path.join(dst, "BLACKBOX.bin"))
    entry.update(image_bytes=size, image_sha256=sha, scope="fast" if a.fast else "full")
    files = [f for f in test_files() if applicable(test_info(f), sets, f) and (not a.only or f in a.only)]
    if a.fast:                                                          # the tests that look at hooks, only: the others run on `all` and do not depend on which hooks are in
        sens = json.load(open(os.path.join(ROOT, "docs", "test-sensitivity.json")))["hook_sensitive"]
        files = [f for f in files if (f in sens or f in PS.TEST_PAGES) and f not in PS.SLOW_CORE_ONLY]
    skipped = [f for f in test_files() if test_info(f)["kind"] == "image" and f not in files]
    print(f"{cid}: {len(sets)} sets, image {size} bytes, cave {cave_n}/{PS.CAVE_LIMIT} ({100 * cave_n / PS.CAVE_LIMIT:.0f}%); {len(files)} tests apply, {len(skipped)} n/a")
    wf = wiring_check(open(os.path.join(dst, "BLACKBOX.bin"), "rb").read(), sets)
    print("  " + ("pass" if not wf else "FAIL") + " wiring (every hook of the chosen sets present, every other set's sites stock)" + ("  " + "; ".join(wf[:3]) if wf else ""))
    res = run_all(files, cid, os.path.join(dst, "BLACKBOX.bin"), sets, os.path.join(dst, "logs"), a.jobs)
    if hashlib.sha256(open(os.path.join(dst, "BLACKBOX.bin"), "rb").read()).hexdigest() != sha:
        res["image-unchanged"] = dict(status="FAIL", seconds=0, fails=["the image under test was changed while the tests ran"])
    res["wiring"] = dict(status="pass" if not wf else "FAIL", seconds=0, fails=wf[:3])
    note = blob_restore_if_changed()
    if note:
        print("NOTE:", note)
    entry["tests"] = {f: r["status"] for f, r in res.items()}
    sens = json.load(open(os.path.join(ROOT, "docs", "test-sensitivity.json"))) if os.path.exists(os.path.join(ROOT, "docs", "test-sensitivity.json")) else None
    entry["hook_blind_tests"] = sorted(set(sens["hook_blind"]) & set(files)) if sens else "unknown: run `combo.py control`"
    entry["n_a"] = skipped
    failed = [f for f, r in res.items() if r["status"] != "pass"]
    entry["status"] = "pass" if files and not failed and not a.only else ("partial" if a.only else ("fail" if failed else "untested"))
    if entry["status"] == "pass":
        hostrec = (json.load(open(REGISTRY)).get("host") if os.path.exists(REGISTRY) else None) or {}
        entry["host_tests"] = hostrec.get("status", "not recorded: run `combo.py host --record`")
    json.dump(entry, open(os.path.join(dst, "report.json"), "w"), indent=1)
    print(f"{cid}: {entry['status'].upper()} ({len(files) - len(failed)}/{len(files)} tests passed)" + (f"; failed: {' '.join(failed)}" if failed else ""))
    if a.record and not a.only:
        record(entry)
    return 0 if entry["status"] == "pass" else 1


def cmd_host(a):
    cave_n, cave_sha = ensure_cave()
    files = [f for f in test_files() if test_info(f)["kind"] == "host"]
    print(f"host tests (no image): {len(files)}")
    res = run_all(files, "_host", os.path.join(OUT, "cave.bin"), ["cave"], os.path.join(OUT, "combos", "_host", "logs"), a.jobs)
    failed = [f for f, r in res.items() if r["status"] != "pass"]
    status = "pass" if not failed else "fail"
    print("host tests:", status.upper(), f"({len(files) - len(failed)}/{len(files)})", " ".join(failed))
    if a.record:
        reg = json.load(open(REGISTRY)) if os.path.exists(REGISTRY) else dict(combos={})
        reg["host"] = dict(status=status, tests={f: r["status"] for f, r in res.items()}, tested_at=datetime.date.today().isoformat(), source_rev=git_rev(), cave_sha256=cave_sha)
        json.dump(reg, open(REGISTRY, "w"), indent=1, sort_keys=True)
        open(REGISTRY, "a").write("\n")
    return 0 if not failed else 1


def cmd_list(a):
    print("sets (canonical order):")
    for s in PS.CANON:
        d = PS.SETS[s]
        print(f"  {s:9s} {d['feature']:7s} requires {','.join(d['requires']) or '-':14s} {'MCL ' if d.get('mcl') else ''}{d['title']}")
    print("presets:")
    for p, d in PS.PRESETS.items():
        print(f"  {p:8s} {len(resolve([p])):2d} sets  {d['title']}")
    print("tests:")
    for f in test_files():
        i = test_info(f)
        print(f"  {f:26s} {i['kind']:5s} " + " | ".join("+".join(x for x in im if x not in PS.CORE) for im in i["images"]))


def cmd_resolve(a):
    sets = resolve(a.names)
    print(" ".join(sets))


def cmd_build(a):
    ensure_cave()
    sets = resolve(a.names)
    cid = combo_id(sets, a.names)
    image, sha, size = build(sets)
    dst = os.path.join(OUT, "combos", cid)
    os.makedirs(dst, exist_ok=True)
    shutil.copy2(image, os.path.join(dst, "BLACKBOX.bin"))
    print(f"{dst}/BLACKBOX.bin  {size} bytes  sha256 {sha[:16]}  sets: {' '.join(sets)}")


def cmd_verify_order(a):
    ensure_cave()
    sets = [s for s in PS.CANON if not PS.SETS[s].get("mcl")]
    rng = random.Random(1)
    hashes = {}
    orders = [sets, ["cave"] + [s for s in sets if s != "cave"], sets[::-1]] + [rng.sample(sets, len(sets)) for _ in range(a.n)]
    for o in orders:
        _, sha, _ = build(o)
        hashes.setdefault(sha, []).append(o)
        shutil.rmtree(os.path.join(OUT, "+".join(o)), ignore_errors=True)
    print(f"{len(sets)} sets, {len(orders)} orders (canonical, cave first, reversed, {a.n} random): {len(hashes)} distinct image(s)")
    return 0 if len(hashes) == 1 else 1


def cmd_table(a):
    ensure_cave()
    import importlib
    sys.path.insert(0, os.path.join(ROOT, "patches"))
    rows = []
    for s in PS.CANON:
        if PS.SETS[s].get("mcl") and not blob_ok()[0]:
            rows.append((s, None, None))
            continue
        m = importlib.import_module(s)
        rows.append((s, len(m.PATCHES), sum(len(n) for _, _, n in m.PATCHES)))
    cave_n = len(open(os.path.join(OUT, "cave.bin"), "rb").read())
    L = ["# Patch sets, dependencies and presets", "",
         "Generated by `python3 tools/combo.py table` from `tools/patchsets.py`. Do not edit by hand.", "",
         "All the code is compiled once into one block (the cave). A set only adds the hooks that point into it, so choosing sets never makes the image smaller:",
         f"the cave is {cave_n:,} of {PS.CAVE_LIMIT:,} bytes ({100 * cave_n / PS.CAVE_LIMIT:.0f}%, {PS.CAVE_LIMIT - cave_n:,} free); the build fails if it overflows. "
         "The MCL program is a separate block (~402 KB, MCL 5.10+).", "",
         "## Sets", "", "Order: the table order is the order `patch.py` takes and the web patcher applies. The order does not change a byte of the image except `cave` -> `mclblob` -> `mclsd` "
         "(`python3 tools/combo.py verify-order`).", "",
         "| set | feature | requires | hooks | bytes | what |", "|---|---|---|---|---|---|"]
    for s, n, b in rows:
        d = PS.SETS[s]
        L.append(f"| `{s}` | {d['feature']} | {', '.join('`' + r + '`' for r in d['requires']) or '-'} | {n if n is not None else '(blob)'} | {b if b is not None else '(blob)'} | {d['title']} |")
    L += ["", "Notes:", ""]
    for s in PS.CANON:
        if PS.SETS[s].get("note"):
            L.append(f"- `{s}`: {PS.SETS[s]['note']}")
    L += ["", "Dependency evidence: every `requires` comes from the module's own docstring and the source (`evidence` in `tools/patchsets.py`). No test builds a set "
          "WITHOUT what it requires, so the rule is the safe one, not a proven minimum: combinations that break it are never offered.", "",
          "## Presets", "", "| preset | sets | what |", "|---|---|---|"]
    for p, d in PS.PRESETS.items():
        L.append(f"| `{p}` | {len(resolve([p]))} | {d['title']}{' (experimental)' if d.get('experimental') else ''} |")
    L += ["", "The 3.1.n features (`" + "`, `".join(PS.CORE) + "`) are one bundle in every combination: the tests only cover them together.", ""]
    L += ["## Parts and add-ons (free combinations)", "",
          "Pick any parts; the page leaves out whatever is not picked. All the pages are tabs of the Looper page, so any part brings the `looper` hooks (the host) and the others get a `hide_*` set "
          "(`bb_hide` in the cave: the SONG round, BACK, MIX and the pad recorder's Rec Input list skip a page that is hidden). Every part works alone or with any other.", "",
          "| part | sets | left out with |", "|---|---|---|"]
    for k, d in PS.FEATURES.items():
        L.append(f"| {d['title']} | `looper`" + "".join(f", `{x}`" for x in d["sets"]) + f" | {('`' + d['hide'] + '`') if d['hide'] else 'not adding its sets (no blob, no page)'} |")
    L += ["", "| add-on | sets |", "|---|---|"]
    for k, d in PS.ADDONS.items():
        L.append(f"| {d['title']} (`{k}`) | " + ", ".join(f"`{x}`" for x in d["sets"]) + " |")
    L += ["", "Rules the page enforces (`problems()` in `tools/combo.py`): a set needs what `requires` says; the DRONE-8 page needs `lira`; `looper` with every page hidden shows nothing. "
          "`python3 tools/combo.py resolve parts=samplr,trig addons=tools` prints the sets of a combination.", "",
          "Not decided by the parts yet: the Tools settings page lists all its machine settings whenever `tools` is in (a DRONE-8 Out setting with no DRONE-8 does nothing).", ""]
    if os.path.exists(REGISTRY):
        reg = json.load(open(REGISTRY))
        C = reg["combos"]
        L += ["## Tested (docs/combos.json)", "",
              "`tests` counts the emulator tests that apply plus the wiring check (every hook of the chosen sets present, every other set's sites stock). A test applies when every image it names is made "
              "of the combination's sets and the pages it walks are in the image; it then runs on the combination's own image. Scope `full` = every applicable test; `fast` = the hook-sensitive tests "
              "and the page tests only (the other tests do not depend on which hooks are in: they pass on an image with none, and run in full on the presets and the single parts).", "",
              "### Presets", "", "| preset | status | tests | tested | source |", "|---|---|---|---|---|"]
        for cid in PS.PRESETS:
            e = C.get(cid)
            if e:
                t = e.get("tests", {})
                why = f" - {e['reason']}" if e["status"] == "untested" else ""
                L.append(f"| `{cid}` | **{e['status']}**{why} | {sum(v == 'pass' for v in t.values())}/{len(t)} | {e['tested_at']} | `{e['source_rev']}` |")
        free = {k: e for k, e in C.items() if "parts" in e}
        ok = sorted(k for k, e in free.items() if e["status"] == "pass")
        un = sorted(k for k, e in free.items() if e["status"] == "untested")
        bad = sorted(k for k, e in free.items() if e["status"] not in ("pass", "untested"))
        L += ["", f"### Free combinations: {len(ok)} pass, {len(un)} untested (MCL: no blob here), {len(bad)} other", "",
              "| parts | add-ons | status | scope | tests |", "|---|---|---|---|---|"]
        for k in sorted(free, key=lambda k: (free[k]["status"] != "pass", "mcl" in free[k]["parts"], len(free[k]["parts"]), k)):
            e = free[k]
            t = e.get("tests", {})
            ad = "none" if not e["addons"] else ("all five" if len(e["addons"]) == 5 else ", ".join(e["addons"]))
            L.append(f"| {', '.join(e['parts']) or '-'} | {ad} | **{e['status']}** | {e.get('scope', '-')} | {sum(v == 'pass' for v in t.values())}/{len(t)} |")
        if os.path.exists(os.path.join(ROOT, "docs", "test-sensitivity.json")):
            sn = json.load(open(os.path.join(ROOT, "docs", "test-sensitivity.json")))
            L += ["", f"**What a pass proves.** {len(sn['hook_blind'])} of the {len(sn['hook_blind']) + len(sn['hook_sensitive'])} image tests still pass on an image with NO hooks "
                  "(stock + the code block): they drive the cave's functions directly. They show the code works, not that a combination's hooks reach it "
                  "(that is what the wiring check and the hook-sensitive tests are for). The list is `docs/test-sensitivity.json` (`python3 tools/combo.py control`). "
                  "The page logic of every combination of hidden pages (SONG round, BACK, MIX, Rec Input list) is `test_pages.py`, in the host tier. Nothing here has been run on the unit."]
        if reg.get("host"):
            h = reg["host"]
            L.append(f"\nHost tests (no image; they test the C source and `test_pages.py`): **{h['status']}**, {sum(v == 'pass' for v in h['tests'].values())}/{len(h['tests'])}, {h['tested_at']}, `{h['source_rev']}`.")
    open(os.path.join(ROOT, "docs", "patch-sets.md"), "w").write("\n".join(L) + "\n")
    print("wrote docs/patch-sets.md")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    for name, fn in (("list", cmd_list), ("table", cmd_table)):
        sp.add_parser(name).set_defaults(fn=fn)
    for name, fn in (("resolve", cmd_resolve), ("build", cmd_build), ("test", cmd_test)):
        p = sp.add_parser(name)
        p.add_argument("names", nargs="+")
        p.set_defaults(fn=fn)
        if name == "test":
            p.add_argument("-j", "--jobs", type=int, default=os.cpu_count())
            p.add_argument("--only", nargs="+")
            p.add_argument("--record", action="store_true")
            p.add_argument("--fast", action="store_true", help="only the hook-sensitive tests and the page tests (docs/test-sensitivity.json)")
    p = sp.add_parser("host")
    p.add_argument("-j", "--jobs", type=int, default=os.cpu_count())
    p.add_argument("--record", action="store_true")
    p.set_defaults(fn=cmd_host)
    p = sp.add_parser("verify")
    p.add_argument("image")
    p.add_argument("names", nargs="+")
    p.set_defaults(fn=cmd_verify)
    p = sp.add_parser("control")
    p.add_argument("-j", "--jobs", type=int, default=os.cpu_count())
    p.set_defaults(fn=cmd_control)
    p = sp.add_parser("verify-order")
    p.add_argument("-n", type=int, default=7)
    p.set_defaults(fn=cmd_verify_order)
    a = ap.parse_args()
    sys.exit(a.fn(a) or 0)


if __name__ == "__main__":
    main()
