#!/usr/bin/env python3
"""tests/tast_layout.py - layout regression for --dump-tast (T2 plan, task 9).

For each struct in tests/tast/layout.c, build a probe with gcc that sets one
member to all ones in a zeroed object and reads back which bits changed, then
compare with the `field` node of the accessor get_<member> in cereal's dump.

usage: tests/tast_layout.py CEREAL FILE.c        env: GCC (default gcc-13)
"""
import os
import re
import subprocess
import sys
import tempfile

STRUCTS = [("S", "get_", "FIELDS"), ("K", "kget_", "KFIELDS"), ("L", "lget_", "LFIELDS")]


def names(text, macro):
    m = re.search(r"#define %s\(X\)(.*?)\n(?!\s)" % macro, text, re.S)
    return re.findall(r"X\((\w+)\)", m.group(1))


def gcc_bits(gcc, path, text):
    prog = ['#include <string.h>', '#include <stdio.h>', '#include "%s"' % os.path.abspath(path),
            "static void rep(const char *n, void *o, unsigned sz) {",
            "  unsigned char *b = o; int first = -1, cnt = 0; unsigned i;",
            "  for (i = 0; i < sz * 8; i++) if (b[i / 8] >> (i % 8) & 1) { if (first < 0) first = i; cnt++; }",
            r'  printf("%s %d %d\n", n, first, cnt);',
            "}",
            "int main(void) {"]
    for sname, _, macro in STRUCTS:
        for f in names(text, macro):
            prog.append("  { struct %s s; memset(&s, 0, sizeof s); s.%s = -1; rep(\"%s.%s\", &s, sizeof s); }"
                        % (sname, f, sname, f))
    prog.append("  return 0;\n}")
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "p.c")
        open(src, "w").write("\n".join(prog))
        subprocess.run([gcc, "-std=gnu11", "-w", "-o", os.path.join(d, "p"), src], check=True,
                       stdin=subprocess.DEVNULL)
        out = subprocess.run([os.path.join(d, "p")], capture_output=True, text=True,
                             stdin=subprocess.DEVNULL).stdout
    return {l.split()[0]: (int(l.split()[1]), int(l.split()[2])) for l in out.splitlines()}


def tast_bits(cereal, path):
    out = subprocess.run([cereal, "check", "--target=x86_64-linux-gnu", "--verify-tast", path],
                         capture_output=True, text=True, stdin=subprocess.DEVNULL).stdout
    res, cur = {}, None
    for line in out.splitlines():
        m = re.match(r"unit \S+ func (\w+) ", line)
        if m:
            cur = m.group(1)
            continue
        if line.startswith("verify:"):
            print(line)
        m = re.match(r"\s+#\d+ field \+(\d+)(?: bit (\d+) width (\d+))? ", line)
        if m and cur and cur not in res:
            res[cur] = (int(m.group(1)) * 8 + int(m.group(2) or 0), int(m.group(3) or 0))
    return res


def main():
    cereal, path = sys.argv[1], sys.argv[2]
    gcc = os.environ.get("GCC", "gcc-13")
    text = open(path).read()
    ref, mine = gcc_bits(gcc, path, text), tast_bits(cereal, path)
    bad = total = 0
    for sname, pre, macro in STRUCTS:
        for f in names(text, macro):
            total += 1
            g = ref["%s.%s" % (sname, f)]
            t = mine.get(pre + f)
            # width 0 in the dump means a whole member: only its start is compared
            ok = t is not None and t[0] == g[0] and (t[1] == 0 or t[1] == g[1])
            if not ok:
                bad += 1
                print("MISMATCH struct %s.%s: gcc first bit %d width %d, cereal %s" % (sname, f, g[0], g[1], t))
    print("layout: %d members, %d mismatches" % (total, bad))
    sys.exit(1 if bad else 0)


main()
