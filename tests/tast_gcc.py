#!/usr/bin/env python3
"""tests/tast_gcc.py - coarse oracle for --dump-tast (docs/TAST_DESIGN.md 6).

For each function that elaborates, compare the multiset of cast target
types in cereal's dump with the casts gcc-13 prints in
-fdump-tree-original.  gcc folds constants and prints `x++` and compound
assignment without their implicit conversions, so a mismatch is a lead, not
a failure: the report lists them and exits 0.

usage: tests/tast_gcc.py CEREAL FILE.c... [-- extra flags]
env:   GCC (default gcc-13)
"""
import collections
import os
import re
import subprocess
import sys
import tempfile

TYPEWORDS = r"(?:unsigned|signed|long|short|int|char|_Bool|void)"
CAST_RE = re.compile(r"\(((?:" + TYPEWORDS + r" ?)+|enum \w+)\)")


def tast_casts(cereal, path, flags):
    out = subprocess.run([cereal, "check", "--target=x86_64-linux-gnu", "--verify-tast"] + flags + [path],
                         capture_output=True, text=True, stdin=subprocess.DEVNULL).stdout
    res, cur = {}, None
    for line in out.splitlines():
        m = re.match(r"unit \S+ func (\w+) ", line)
        if m:
            cur = collections.Counter()
            res[m.group(1)] = cur
            continue
        if line.startswith("unit "):
            cur = None
            continue
        m = re.match(r"\s+#\d+ cast #\d+ (.*)$", line)
        if m and cur is not None:
            cur[m.group(1)] += 1
        if line.startswith("verify:"):
            print("  " + line)
    return res


def gcc_casts(gcc, path, flags):
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "t.c")
        with open(path) as f, open(src, "w") as g:
            g.write(f.read())
        subprocess.run([gcc, "-std=gnu11", "-w", "-c", "-o", "t.o", "-fdump-tree-original"] + flags + [src],
                       capture_output=True, stdin=subprocess.DEVNULL, cwd=d)
        dumps = [n for n in os.listdir(d) if n.endswith(".original")]
        text = open(os.path.join(d, dumps[0])).read() if dumps else ""
    res, cur = {}, None
    for line in text.splitlines():
        m = re.match(r";; Function (\w+) ", line)
        if m:
            cur = collections.Counter()
            res[m.group(1)] = cur
            continue
        if cur is not None:
            for t in CAST_RE.findall(line):
                cur[t.strip()] += 1
    return res


def main():
    args = sys.argv[1:]
    flags = []
    if "--" in args:
        i = args.index("--")
        flags = args[i + 1:]
        args = args[:i]
    cereal, files = args[0], args[1:]
    gcc = os.environ.get("GCC", "gcc-13")
    agree = total = 0
    for path in files:
        mine = tast_casts(cereal, path, flags)
        ref = gcc_casts(gcc, path, flags)
        for fn in sorted(mine):
            total += 1
            a, b = mine[fn], ref.get(fn)
            if b is None:
                print("%s %s: no gcc function" % (path, fn))
            elif a == b:
                agree += 1
            else:
                print("%s %s: only cereal %s; only gcc %s" %
                      (path, fn, dict(a - b), dict(b - a)))
    print("agree %d of %d functions" % (agree, total))


main()
