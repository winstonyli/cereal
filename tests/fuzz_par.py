#!/usr/bin/env python3
"""Differential fuzzer: sequential vs parallel -E on random programs built to
put macro invocations, directives, #line, includes, _Pragma and comments
across segment splits.  usage: fuzz_par.py BIN [N] [SEED]"""
import os, random, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_pp import gen_program  # noqa: E402

BIN = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 200
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 1

def run(args, path):
    p = subprocess.run([BIN, "-E"] + args + [path], capture_output=True, timeout=30)
    return p.returncode, p.stdout, p.stderr

def main():
    r = random.Random(SEED)
    d = tempfile.mkdtemp()
    fails = 0
    for it in range(N):
        gen_program(r, d)
        path = os.path.join(d, "main.c")
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
