# merge_builtin_pfx.py TABLE PFX: regenerate the gnu = 2 entries of
# src/c/cbuiltin_tab.h from the output of gen_builtin_pfx.py (PFX), keeping
# those whose types bt_type (cexpr.c) parses; every other entry stays.  Prints
# the new table on stdout.
import re, sys

PARSED = {"void", "int", "char", "long int", "long long int",
          "long unsigned int", "double", "float", "long double", "__float128",
          "unsigned int", "short unsigned int", "long long unsigned int",
          "unsigned char", "__int128 unsigned", "_Bool", "__va_list_tag"}


def ok(t):
    t = t.strip()
    if t.startswith("const "):
        t = t[6:]
    while t.endswith("*"):
        t = t[:-1].strip()
    if t.startswith("_Complex "):
        t = t[9:]
    return t in PARSED or t in ("...", "?", "")


# type-generic or typed elsewhere (overflow_func_type), or va_list tricks
SKIP = re.compile(r"overflow|speculation_safe_value_(1|2|4|16)$|^(sysv_)?va_(start|end|copy)$")
pat = re.compile(r'^    \{"([^"]*)", "[^"]*", \d, "([^"]*)", (\d)\},$')
head, ents, tail = [], [], []
for l in open(sys.argv[1], encoding="utf-8").read().split("\n"):
    m = pat.match(l)
    if m:
        ents.append((m.group(1), l, int(m.group(3))))
    elif ents:
        tail.append(l)
    else:
        head.append(l)
keep = [(n, l) for n, l, g in ents if g != 2]
new = []
for l in open(sys.argv[2], encoding="utf-8"):
    l = l.rstrip("\n")
    m = pat.match(l)
    if m and not SKIP.search(m.group(1)) and \
            all(ok(t) for t in m.group(2).split("|")):
        new.append((m.group(1), l))
allv = sorted(keep + new, key=lambda e: e[0].encode())
sys.stdout.buffer.write(
    "\n".join(head + [l for _, l in allv] + tail).encode("utf-8"))
print(len(new), "gnu = 2 entries", file=sys.stderr)
