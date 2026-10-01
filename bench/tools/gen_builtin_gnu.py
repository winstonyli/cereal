import re, subprocess, sys
cc1 = subprocess.check_output(["gcc-13", "-print-prog-name=cc1"]).decode().strip()
out = subprocess.check_output(["strings", cc1]).decode().split("\n")
cands = sorted({l[10:] for l in out if re.fullmatch(r"__builtin_[A-Za-z_][A-Za-z0-9_]*", l)})
cands = [x for x in cands if not x.startswith("_") ]
open("/tmp/gg_decl.c", "w").write("".join("void %s(int,int,int,int,int,int,int,int,int);\n" % x for x in cands))
def conflicts(std):
    r = subprocess.run(["gcc-13", "-fsyntax-only", "-std=" + std, "/tmp/gg_decl.c"], capture_output=True, text=True)
    d = {}
    for m in re.finditer(r"conflicting types for built-in function ‘(\w+)’; expected ‘(.*)’", r.stderr):
        d[m.group(1)] = m.group(2)
    return d
g, c = conflicts("gnu99"), conflicts("c99")
only = sorted(set(g) - set(c))
print(len(cands), len(g), len(c), len(only), file=sys.stderr)
# implicit-declaration mismatch flag
open("/tmp/gg_imp.c", "w").write("".join("void t%d(void){ %s(); }\n" % (i, x) for i, x in enumerate(only)))
r = subprocess.run(["gcc-13", "-fsyntax-only", "-std=gnu99", "/tmp/gg_imp.c"], capture_output=True, text=True)
mism = set(re.findall(r"incompatible implicit declaration of built-in function ‘(\w+)’", r.stderr))
def sig(s):
    s = s.strip()
    # split RET(ARGS) at the last top-level '(' group
    depth = 0
    for i in range(len(s) - 1, -1, -1):
        if s[i] == ")":
            depth += 1
        elif s[i] == "(":
            depth -= 1
            if depth == 0:
                break
    ret, args = s[:i], s[i + 1:-1]
    parts = [a.strip() for a in args.split(",")] if args.strip() != "void" else []
    return "|".join([ret.strip()] + [p for p in parts if p])
for x in only:
    print('    {"%s", "", %d, "%s", 1},' % (x, 1 if x in mism else 0, sig(g[x])))
