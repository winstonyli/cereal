#!/usr/bin/env python3
"""Differential fuzzer: sequential vs parallel -E on random programs built to
put macro invocations, directives, #line, includes, _Pragma and comments
across segment splits.  usage: fuzz_par.py BIN [N] [SEED]"""
import os, random, subprocess, sys, tempfile

BIN = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 200
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 1

NAMES = ["A", "B", "F", "G", "H", "E", "X"]

def gen_line(r, depth):
    k = r.randrange(24)
    n = r.choice(NAMES)
    if k == 0:
        return "#define %s(x, ...) x + %s(__VA_ARGS__) %s" % (n, r.choice(NAMES), r.choice(["", "#x", "x ## 1"]).replace("#x", "") )
    if k == 1:
        return "#define %s %s" % (n, r.choice(["F", "G(", ")", "1 +", "A B", "", "(x)", "__LINE__", "__FILE__"]))
    if k == 2:
        return "#undef %s" % n
    if k == 3:
        return "#if %s\n%s\n#else\n%s\n#endif" % (r.choice(["1", "0", "defined(%s)" % n, "%s + 1" % n]), gen_line(r, depth), gen_line(r, depth))
    if k == 4:
        return "#line %d%s" % (r.randrange(1, 500), r.choice(["", ' "fake.c"', ' "other.h"']))
    if k == 5 and depth < 2:
        return '#include "inc%d.h"' % r.randrange(3)
    if k == 6:
        return "_Pragma(\"omp parallel\") x"
    if k == 7:
        return "#pragma once_not %d" % r.randrange(9)
    if k == 8:
        return "/* multi\n   line %d */ y" % r.randrange(9)
    if k == 9:
        return "%s(" % n
    if k == 10:
        return ")"
    if k == 11:
        return "   \t  "
    if k == 12:
        return ""
    if k == 13:
        return "s = \"str %d\"; // c" % r.randrange(9)
    if k == 14:
        return "long \\\n  splice;"
    if k == 15:
        return "#define %s(a) (a) * %s(a)" % (n, r.choice(NAMES))
    if k == 16:
        return "#ifdef %s\n%s(1, 2)\n#endif" % (n, n)
    return " ".join(r.choice(NAMES + ["(", ")", ",", "+", "1", "x", "a##b", "#"]) for _ in range(r.randrange(1, 8)))

def gen_file(r, lines, depth):
    return "\n".join(gen_line(r, depth) for _ in range(lines)) + "\n"

def run(args, path):
    p = subprocess.run([BIN, "-E"] + args + [path], capture_output=True, timeout=30)
    return p.returncode, p.stdout, p.stderr

def main():
    r = random.Random(SEED)
    d = tempfile.mkdtemp()
    fails = 0
    for it in range(N):
        for i in range(3):
            with open(os.path.join(d, "inc%d.h" % i), "w") as f:
                f.write(gen_file(r, r.randrange(1, 12), 2))
        path = os.path.join(d, "main.c")
        with open(path, "w") as f:
            f.write(gen_file(r, r.randrange(5, 80), 0))
        seq = run(["-fparallel=off"], path)
        for t, c, w in ((1, 1, 0), (2, 1, 0), (4, 1, 0), (4, 1, 1),
                        (3, 17, 2), (8, 3, 0)):
            par = run(["-fparallel=on", "-fparallel-threads=%d" % t,
                       "-fparallel-chunk=%d" % c,
                       "-fparallel-window=%d" % w], path)
            if par != seq:
                fails += 1
                keep = os.path.join(d, "fail%d" % it)
                os.makedirs(keep, exist_ok=True)
                for fn in ["main.c", "inc0.h", "inc1.h", "inc2.h"]:
                    with open(os.path.join(d, fn)) as a, open(os.path.join(keep, fn), "w") as b:
                        b.write(a.read())
                what = "out" if par[1] != seq[1] else "diag" if par[2] != seq[2] else "rc"
                print("FAIL %s (threads=%d chunk=%d window=%d): %s"
                      % (what, t, c, w, keep))
                break
    print("fuzz_par: %d/%d programs differ" % (fails, N))
    sys.exit(1 if fails else 0)

main()
