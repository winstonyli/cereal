#!/usr/bin/env python3
"""Per-unit summaries and read sets (src/c/csum.c): relational tests.

usage: summary.py CEREAL TMPDIR

Each case writes small C files, runs `cereal check --dump-summaries` (and
--validate-summaries=), and compares.  Prints one line per failure; exit
status 1 if any failed."""
import os
import re
import subprocess
import sys

cereal, tmp = sys.argv[1], sys.argv[2]
fails = []
n = 0


def run(src, *flags, name="t.c"):
    path = os.path.join(tmp, name)
    with open(path, "w") as f:
        f.write(src)
    p = subprocess.run([cereal, "check", "-std=c99", *flags, path],
                       capture_output=True, text=True)
    return p.stdout


def dump(src, name="t.c"):
    return run(src, "--dump-summaries", name=name)


def units(out):
    """The dump as a list of (header, entries, reads), the unit index
    dropped from the header."""
    res = []
    for line in out.splitlines():
        if line.startswith("unit "):
            res.append([re.sub(r"^unit \d+ ", "", line), [], []])
        elif line.startswith("  read "):
            res[-1][2].append(line)
        elif line.startswith("  "):
            res[-1][1].append(line)
    return res


def unit_with(out, decl):
    """The unit whose entries include a line starting `  ord ... decl`."""
    for u in units(out):
        for e in u[1]:
            if re.match(r"  \w+ \w+ %s\b" % re.escape(decl), e):
                return u
    raise KeyError(decl)


def validate(old_src, new_src):
    """Per unit verdicts of old's read sets in new's states."""
    with open(os.path.join(tmp, "old.sum"), "w") as f:
        f.write(dump(old_src, "old.c"))
    out = run(new_src, "--validate-summaries=" + os.path.join(tmp, "old.sum"),
              name="new.c")
    return re.findall(r"validate unit (\d+): (valid|invalid|no summary)", out)


def check(cond, what):
    global n
    n += 1
    if not cond:
        fails.append(what)


USE = "struct Q { int x; };\nint use(struct Q *q) { return q->x + K; }\n"

# 1. the same unit text in different surrounding context
a = dump("enum { K = 4 };\n" + USE)
b = dump("int unrelated1;\ntypedef long L;\nenum { K = 4 };\nstruct Z { L z; };\n" + USE)
ua, ub = unit_with(a, "use"), unit_with(b, "use")
check(ua == ub, "context: function unit differs:\n%s\n%s" % (ua, ub))
qa = [u for u in units(a) if any(" Q " in e for e in u[1])][0]
qb = [u for u in units(b) if any(" Q " in e for e in u[1])][0]
check(qa == qb, "context: struct unit differs")

# 2. a body-only edit leaves the signature summary alone
b1 = dump("int g;\nint f(int a) { return a; }\n")
b2 = dump("int g;\nint f(int a) { int t = a * 3; return t + g; }\n")
u1, u2 = unit_with(b1, "f"), unit_with(b2, "f")
check(u1[0] == u2[0] and u1[1] == u2[1],
      "body edit changed the summary:\n%s\n%s" % (u1, u2))
check(u1[2] != u2[2], "body edit did not change the read set")

# 3. reordering unrelated declarations
r1 = dump("int a1;\nlong b1;\nstruct S1 { int m; };\nint fn(void) { return 0; }\n")
r2 = dump("int fn(void) { return 0; }\nstruct S1 { int m; };\nlong b1;\nint a1;\n")
check(sorted(map(str, units(r1))) == sorted(map(str, units(r2))),
      "reordering changed the units' summaries")

# 4. read sets: name-only reads, layout reads
SRC = ("struct S { int a; };\nint g;\nint *pg(struct S *s) { return &g; }\n"
       "int sz(void) { return (int)sizeof(struct S) + g; }\n")
old = SRC
same = validate(old, old)
check(all(v == "valid" for _, v in same) and len(same) == 4, "self validation: %s" % same)
# S gains a member: only the layout reader is invalid
new = SRC.replace("int a;", "int a; int b;")
v = dict(validate(old, new))
check(v == {"0": "valid", "1": "valid", "2": "valid", "3": "invalid"},
      "layout change: %s" % v)
# g changes type: the unit defining it still sees the same entry state;
# the readers (pg takes &g, sz reads g) do not
new = SRC.replace("int g;", "long g;")
v = dict(validate(old, new))
check(v == {"0": "valid", "1": "valid", "2": "invalid", "3": "invalid"},
      "type change: %s" % v)
# the declaration of g vanishes: a miss
new = SRC.replace("int g;\n", "").replace("&g", "0").replace(" + g", "")
v = dict(validate(old, new))
check(v == {"0": "valid", "1": "valid", "2": "invalid"}, "g removed: %s" % v)
# early cutoff: editing pg's body leaves sz valid, and a function unit
# reading pg stays valid
old = "int pg(void);\nint user(void) { return pg(); }\nint pg(void) { return 1; }\n"
new = "int pg(void);\nint user(void) { return pg(); }\nint pg(void) { return 2 + 3; }\n"
v = dict(validate(old, new))
check(all(x == "valid" for x in v.values()), "body edit cutoff: %s" % v)
# a signature edit is not
new = old.replace("int pg(void);", "long pg(void);").replace("int pg(void) {", "long pg(void) {")
v = dict(validate(old, new))
check(v["1"] == "invalid", "signature edit: %s" % v)
# a new conflicting declaration appears where the unit used to declare a name
old = "int x;\nint x;\n"
new = "long x;\nint x;\n"
check(dict(validate(old, new)) == {"0": "valid", "1": "invalid"}, "redeclaration read")
# enumerator values are part of what readers see
old = "enum { A = 1 };\nint f(void) { return A; }\n"
new = "enum { A = 2 };\nint f(void) { return A; }\n"
check(dict(validate(old, new)) == {"0": "valid", "1": "invalid"}, "enumerator value")
# an incomplete struct completed later: name read valid, layout read not
old = "struct T;\nstruct T *p;\nint f(void) { return (int)sizeof(struct T); }\n"
new = "struct T { int a; };\nstruct T *p;\nint f(void) { return (int)sizeof(struct T); }\n"
v = dict(validate(old, new))
check(v["2"] == "invalid", "incomplete -> complete layout read: %s" % v)

for f in fails:
    print("FAIL: summary: " + f)
sys.exit(1 if fails else 0)
