"""fx.py <addr> [regex] [context]  — print a function from out_all.c without local declarations; optional grep with context."""
import re, sys
txt = open(__file__.rsplit("/", 1)[0] + "/out_all.c").read()
funcs = {f.split("\n", 1)[0].split()[0]: f for f in re.split(r"(?m)^// ===== ", txt)[1:]}
decl = re.compile(r"^\s*(undefined\d?|int|uint|char|short|ushort|byte|bool|float|double|longlong|ulonglong|code|float10) \**\w+( \[\d+\])?;$")
lines = [l for l in funcs[sys.argv[1]].splitlines() if not decl.match(l)]
if len(sys.argv) > 2:
    ctx = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    hits = [k for k, l in enumerate(lines) if re.search(sys.argv[2], l)]
    last = -1
    for k in hits:
        lo = max(k - ctx, last + 1)
        if lo > last + 1: print("    ...")
        print("\n".join(lines[lo:k + ctx + 1])); last = k + ctx
else:
    print("\n".join(lines))
