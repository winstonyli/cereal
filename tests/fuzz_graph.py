#!/usr/bin/env python3
"""Macro-graph soundness fuzzer: `cereal index --check-graph` on random
programs (tests/gen_pp.py).  Every expansion must lie in the static closure
of its file-level invocation; violations are reduced and kept.
usage: fuzz_graph.py BIN [N] [SEED] [KEEPDIR]"""
import os, random, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gen_pp import gen_program  # noqa: E402

BIN = os.path.abspath(sys.argv[1])
N = int(sys.argv[2]) if len(sys.argv) > 2 else 100
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 1
KEEP = sys.argv[4] if len(sys.argv) > 4 else tempfile.mkdtemp()
FILES = ["main.c", "inc0.h", "inc1.h", "inc2.h"]


def violates(d):
    try:
        p = subprocess.run([BIN, "index", "--check-graph", "main.c"], cwd=d,
                           capture_output=True, timeout=10)
    except subprocess.TimeoutExpired:
        return True
    return b"graph: " in p.stdout and not p.stdout.rstrip().endswith(
        b" 0 outside their closure")


def reduce(d):
    for fn in FILES:
        path = os.path.join(d, fn)
        lines = open(path).read().split("\n")
        n = max(len(lines) // 2, 1)
        while n >= 1:
            i = 0
            while i < len(lines):
                trial = lines[:i] + lines[i + n:]
                with open(path, "w") as f:
                    f.write("\n".join(trial))
                if violates(d):
                    lines = trial
                else:
                    i += n
            with open(path, "w") as f:
                f.write("\n".join(lines))
            n //= 2


def main():
    r = random.Random(SEED)
    found = 0
    for it in range(N):
        d = os.path.join(KEEP, "case%04d" % it)
        os.makedirs(d, exist_ok=True)
        gen_program(r, d)
        if not violates(d):
            shutil.rmtree(d)
            continue
        found += 1
        reduce(d)
        print("VIOLATION: %s" % d)
    print("fuzz_graph: %d/%d programs violate the graph" % (found, N))
    sys.exit(1 if found else 0)


main()
