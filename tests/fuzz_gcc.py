#!/usr/bin/env python3
"""GCC-fidelity fuzzer: `cereal -E` vs `gcc -std=c99 -E` on random programs
(tests/gen_pp.py), token by token (tests/tokdiff.py) plus error parity (both
fail or both succeed).  Each mismatch is reduced line by line and kept.
usage: fuzz_gcc.py BIN [N] [SEED] [KEEPDIR]"""
import os, random, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gen_pp import gen_program  # noqa: E402

BIN = os.path.abspath(sys.argv[1])
N = int(sys.argv[2]) if len(sys.argv) > 2 else 100
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 1
KEEP = sys.argv[4] if len(sys.argv) > 4 else tempfile.mkdtemp()
FILES = ["main.c", "inc0.h", "inc1.h", "inc2.h"]


def mismatch(d):
    """None if cereal agrees with gcc, else a short reason."""
    ref = subprocess.run(["gcc", "-std=c99", "-E", "main.c"], cwd=d,
                         capture_output=True)
    try:
        out = subprocess.run([BIN, "-E", "-fparallel=off", "main.c"], cwd=d,
                             capture_output=True, timeout=10)
    except subprocess.TimeoutExpired:
        return "timeout"
    if out.returncode > 1:
        return "crash"
    # GCC bug not reproduced: after a malformed _Pragma it prints the name
    # glued to the next token ("_Pragmaa"), which lexes as one identifier.
    ref_out = re.sub(rb"\b_Pragma(?=[A-Za-z0-9_$\"'])", b"_Pragma ", ref.stdout)
    with open(os.path.join(d, "ref.i"), "wb") as f:
        f.write(ref_out)
    with open(os.path.join(d, "out.i"), "wb") as f:
        f.write(out.stdout)
    if subprocess.run([sys.executable, os.path.join(HERE, "tokdiff.py"),
                       os.path.join(d, "ref.i"), os.path.join(d, "out.i")],
                      capture_output=True).returncode:
        return "tokens"
    if (ref.returncode != 0) != (out.returncode != 0):
        return "status"
    return None


def reduce(d, why):
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
                if mismatch(d) == why:
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
        why = mismatch(d)
        if why is None:
            shutil.rmtree(d)
            continue
        found += 1
        reduce(d, why)
        print("MISMATCH %s: %s" % (why, d))
    print("fuzz_gcc: %d/%d programs differ from gcc" % (found, N))
    sys.exit(1 if found else 0)


main()
