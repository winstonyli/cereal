import re, subprocess, sys
cc1 = subprocess.check_output(["gcc-13", "-print-prog-name=cc1"]).decode().strip()
out = subprocess.check_output(["strings", cc1]).decode().split("\n")
cands = sorted({l[10:] for l in out if re.fullmatch(r"__builtin_[A-Za-z_][A-Za-z0-9_]*", l)})
cands = [x for x in cands if not x.startswith("_")]
decl = "".join("void __builtin_%s(int,int,int,int,int,int,int,int,int);\n" % x for x in cands)
plain = "".join("void %s(int,int,int,int,int,int,int,int,int);\n" % x for x in cands)
open("/tmp/gp_b.c", "w").write(decl)
open("/tmp/gp_p.c", "w").write(plain)
pat = re.compile(r"conflicting types for built-in function ‘(\w+)’; expected ‘(.*)’")
def conflicts(f, std):
    r = subprocess.run(["gcc-13", "-fsyntax-only", "-std=" + std, f], capture_output=True, text=True)
    return {m.group(1): m.group(2) for m in pat.finditer(r.stderr)}
b = conflicts("/tmp/gp_b.c", "c99")
p = conflicts("/tmp/gp_p.c", "gnu99")
only = sorted(x for x in cands if "__builtin_" + x in b and x not in p)
def sig(s):
    s = s.strip()
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
    return "|".join([ret.strip()] + [q for q in parts if q])
print(len(cands), len(b), len(p), len(only), file=sys.stderr)
for x in only:
    print('    {"%s", "", 0, "%s", 2},' % (x, sig(b["__builtin_" + x]) + ("|" if "|" not in sig(b["__builtin_" + x]) else "")))
