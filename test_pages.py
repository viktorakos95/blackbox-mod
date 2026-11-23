"""Which pages an image has (src/looper_page.c bb_host / bb_hide, patches/hide_*.py, patches/cave.py): for EVERY combination of hidden pages, with and without
the MCL page, the SONG round forward and back, the first page a stock screen opens, and the pad recorder's Rec Input list. It builds its own images (patch.py),
so it belongs to no combination: it covers the page logic of all of them.
NOT checked: the screens themselves (the page drawing is tested by the page tests on the default image)."""
import itertools
import os
import subprocess
import sys

from emu import Emu
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_SP

CORE = "solo slice duck chord cond filter cpu od comp seqfix fx2 munchi".split()
HIDE = ["hide_looper", "hide_samplr", "hide_trig", "hide_drone"]
TABS = (0, 5, 6, 7, 8)
fails = 0
n_checks = 0


def check(name, ok, info=""):
    global fails, n_checks
    n_checks += 1
    if not ok:
        fails += 1
        print("FAIL " + name + ("  " + str(info) if info else ""))


def build(names):
    subprocess.run([sys.executable, "patch.py"] + names, check=True, capture_output=True)
    return "out/" + "+".join(names) + "/BLACKBOX.bin"


def s32(v):
    return v - (1 << 32) if v >= 1 << 31 else v


for hidden in itertools.product((0, 1), repeat=4):
    names = CORE + ["looper"] + [h for h, x in zip(HIDE, hidden) if x] + ["lira", "lirarec", "cave"]
    img = build(names)
    for mcl in (0, 1):
        tag = f"hidden={''.join(map(str, hidden))} mcl={mcl}"
        e = Emu(img, "out/cave.elf")
        e.uc.mem_map(0x38800000, 0x1000)
        e.uc.mem_map(0xC0000000, 0x400000)
        if mcl:                                                    # a blob's entry table: magic "MCLB", abi 5
            e.uc.mem_write(0x08120000, b"MCLB" + (5).to_bytes(4, "little"))
            e.w32(e.sym["mcl_entry"], 0x08120000)
        on = {0: not hidden[0], 5: not hidden[1], 6: not hidden[2], 7: not hidden[3], 8: bool(mcl)}
        check(f"{tag}: bb_host is 1 and bb_hide holds the hidden pages", e.r8(e.sym["bb_host"]) == 1 and [e.r8(e.sym["bb_hide"] + i) for i in range(4)] == list(hidden))
        def ret(name, *a):
            e.call(name, *a)
            return s32(e.uc.reg_read(UC_ARM_REG_R0))

        for t in range(9):
            want = on.get(t, on[0]) if t != 4 else on[0]
            check(f"{tag}: page_on({t}) = {int(bool(want))}", bool(ret("looper_page_on", t)) == bool(want), ret("looper_page_on", t))
        E = [t for t in TABS if on[t]]
        check(f"{tag}: next(stock) is the first page", ret("looper_page_next", -1) == (E[0] if E else -1), ret("looper_page_next", -1))
        check(f"{tag}: prev(stock) is the last page", ret("looper_page_prev", -1) == (E[-1] if E else -1), ret("looper_page_prev", -1))
        for i, t in enumerate(E):
            nx = E[i + 1] if i + 1 < len(E) else -1
            pv = E[i - 1] if i else -1
            check(f"{tag}: next({t}) = {nx}", ret("looper_page_next", t) == nx, ret("looper_page_next", t))
            check(f"{tag}: prev({t}) = {pv}", ret("looper_page_prev", t) == pv, ret("looper_page_prev", t))
        if on[0]:
            for t in (1, 2, 3):                                    # the Looper's other tabs count as the Looper page
                check(f"{tag}: next({t}) = next(0)", ret("looper_page_next", t) == ret("looper_page_next", 0))
        # what a stock screen opens: looper_page_want(t) coerces a page the image lacks to the first one it has
        for t in (0, 5, 6, 7, 8):
            before = bytes(e.uc.mem_read(0x38800f00, 0x100))
            e.call("looper_page_want", t)
            after = bytes(e.uc.mem_read(0x38800f00, 0x100))
            diff = [after[i] for i in range(0x100) if after[i] != before[i]]
            exp = (t if on[t] else (E[0] if E else None))
            if exp is None:
                check(f"{tag}: want({t}) with no page changes nothing", not diff, diff)
            else:
                check(f"{tag}: want({t}) opens {exp}", diff == [exp + 1] or (exp + 1 in diff and before[after.index(exp + 1)] == exp + 1), diff)
                e.uc.mem_write(0x38800f00, before)
        # the Rec Input list: DRONE / LOOPER / SAMPLR only when the page of that machine is in the image
        regs = []
        e.w32(0x24000088, 0xC4000000)
        e.w32(0x2400008C, 0xC0000000)
        e.stub(0x0808C0D8, lambda: regs.append((e.arg(3), e.r32(e.uc.reg_read(UC_ARM_REG_SP)))))
        e.call("rec_input_register", 0x24060000, 0x9A, 0, 0, 4, 0)
        ptr, cnt = regs[0]
        got = [e.cstr(e.r32(ptr + 4 * i) & 0xFFFFFFFF) for i in range(cnt)]
        exp_names = ["L+R", "Left", "Right", "Resam"] + [nm for nm, h in (("DRONE", hidden[3]), ("LOOPER", hidden[0]), ("SAMPLR", hidden[1])) if not h]
        check(f"{tag}: Rec Input list", got == exp_names, got)
        srcs = [{"DRONE": 1, "LOOPER": 2, "SAMPLR": 3}[nm] for nm in exp_names[4:]]
        for k in range(3):
            check(f"{tag}: Rec Input choice {4 + k} is source {srcs[k] if k < len(srcs) else 0}", ret("lira_rec_source", 4 + k) == (srcs[k] if k < len(srcs) else 0), ret("lira_rec_source", 4 + k))

# without `looper` there is no page at all, and no recording sources
img = build(CORE + ["lira", "lirarec", "cave"])
e = Emu(img, "out/cave.elf")
check("no looper hooks: bb_host is 0", e.r8(e.sym["bb_host"]) == 0)
for t in (0, 5, 6, 7, 8):
    e.call("looper_page_on", t)
    check(f"no looper hooks: page_on({t}) is 0", e.uc.reg_read(UC_ARM_REG_R0) == 0)
e.uc.mem_map(0xC0000000, 0x400000)
regs = []
e.w32(0x24000088, 0xC4000000)
e.w32(0x2400008C, 0xC0000000)
e.stub(0x0808C0D8, lambda: regs.append((e.arg(3), e.r32(e.uc.reg_read(UC_ARM_REG_SP)))))
e.call("rec_input_register", 0x24060000, 0x9A, 0, 0, 4, 0)
check("no looper hooks: Rec Input list is the stock four", regs[0][1] == 4, regs[0][1])

print(f"{n_checks} checks")
print(f"{fails} failure(s)")
sys.exit(1 if fails else 0)
