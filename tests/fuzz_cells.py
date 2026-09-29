#!/usr/bin/env python3
"""Differential fuzzer for the cell cache: random edit sequences.

`cereal index --replay` processes follow the same random programs through
the same random edits (lines inserted, deleted, replaced, moved, in the
main file and in headers), in pairs: one reusing cells, one sequential
with no cache.  After every edit all rebuild; each build's diagnostics
must be byte-identical within a pair, and so must
  - the query transcript (--transcript: every identifier resolved, its
    references and call hierarchy, every file's refs), which the cached
    side answers by walking cells, and
  - the materialized index (JSON).
Directed cases also check how many cells were reused.
usage: fuzz_cells.py BIN [N] [SEED] [STEPS]"""
import os, random, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_pp import gen_program, line  # noqa: E402

BIN = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 40
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 1
STEPS = int(sys.argv[4]) if len(sys.argv) > 4 else 8
FILES = ["main.c", "inc0.h", "inc1.h", "inc2.h"]


class Replay:
    count = 0

    def __init__(self, d, flags):
        Replay.count += 1
        self.err = os.path.join(d, "stderr%d" % Replay.count)
        self.p = subprocess.Popen(
            [BIN, "index", "--replay", "--all", "-Weverything"] + flags +
            [os.path.join(d, "main.c")], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=open(self.err, "w"),
            env=dict(os.environ, CEREAL_PAR_STATS="1"))
        self.d = d

    def stats(self):
        """(reused, stored) cells: [last build, all builds]"""
        last, total = (0, 0), [0, 0]
        with open(self.err) as f:
            for ln in f:
                if "cells:" in ln:
                    w = ln.split("cells:")[1].split()
                    last = (int(w[0]), int(w[4]))
                    total[0] += last[0]
                    total[1] += last[1]
        return last, tuple(total)

    def build(self):
        self.p.stdin.write(b"x\n")
        self.p.stdin.flush()
        out = []
        while True:
            ln = self.p.stdout.readline()
            if not ln:
                self.p.wait()
                raise SystemExit("replay process died (%d); files in %s"
                                 % (self.p.returncode, self.d))
            if ln == b"=== end\n":
                return b"".join(out)
            out.append(ln)

    def close(self):
        try:
            self.p.stdin.write(b"q\n")
            self.p.stdin.close()
        except OSError:
            pass
        self.p.wait(timeout=30)


def edit(r, d):
    fn = r.choice(FILES) if r.randrange(3) else "main.c"
    path = os.path.join(d, fn)
    with open(path) as f:
        lines = f.read().split("\n")
    depth = 0 if fn == "main.c" else 2
    k = r.randrange(6)
    i = r.randrange(len(lines) + 1)
    if k <= 1 or len(lines) < 3:
        lines[i:i] = line(r, depth).split("\n")
    elif k == 2:
        del lines[min(i, len(lines) - 1)]
    elif k == 3:
        lines[min(i, len(lines) - 1)] = line(r, depth)
    elif k == 4:  # move a block
        j = r.randrange(len(lines))
        n = r.randrange(1, 4)
        blk = lines[j:j + n]
        del lines[j:j + n]
        i = r.randrange(len(lines) + 1)
        lines[i:i] = blk
    else:  # a character-level change
        j = min(i, len(lines) - 1)
        s = lines[j]
        c = r.randrange(len(s) + 1)
        lines[j] = s[:c] + r.choice(["x", " ", "(", ")", ",", "#", "\\"]) + s[c:]
    with open(path, "w") as f:
        f.write("\n".join(lines))
    return fn


# Directed cases: (description, [version of each file per build]).  The
# last build must reuse a cell ("reuse"), reuse every cell ("all") or
# recompute one ("recompute": the key must catch the change).
DIRECTED = [
    # an identical redefinition appears inside a cell, between two reads
    # of X that found one definition before and find two now
    ("same text, different definitions",
     [{"main.c": "#define X 1\n#define F(a) a\nX F(1\n#define Y 2\n) X\n"},
      {"main.c": "#define X 1\n#define F(a) a\nX F(1\n#define X 1\n) X\n"}]),
    # a definition moves without changing: reads still agree, locations
    # of the body must follow it
    ("moved definition",
     [{"main.c": "#define X(a) a + 1\nint v = X(2);\n"},
      {"main.c": "\n\n#define X(a) a + 1\nint v = X(2);\n"}]),
    # __LINE__ depends on lines above the cell
    ("__LINE__ below an edit",
     [{"main.c": "#define L __LINE__\nint a = L;\n"},
      {"main.c": "\n#define L __LINE__\nint a = L;\n"}]),
    # poisoning appears before a cell that uses the name
    ("poison added",
     [{"main.c": "int q;\n#pragma GCC dependency_x\nint r = q;\n"},
      {"main.c": "int q;\n#pragma GCC poison q\nint r = q;\n"}]),
    # nothing changes: every cell is reused, none is stored
    ("no change",
     [{"main.c": "#define F(x) x + 1\nint a = F(1);\nint b = F(2);\n"},
      {"main.c": "#define F(x) x + 1\nint a = F(1);\nint b = F(2);\n"}]),
    # a header changes under an unchanged main file
    ("header edit",
     [{"main.c": '#include "inc0.h"\nint a = H;\n', "inc0.h": "#define H 1\n"},
      {"main.c": '#include "inc0.h"\nint a = H;\n', "inc0.h": "#define H 2\n"}]),
]


EXPECT = {
    "same text, different definitions": "recompute",
    "moved definition": "reuse",
    "__LINE__ below an edit": "recompute",
    "poison added": "recompute",
    "no change": "all",
    "header edit": "recompute",
}

KINDS = (["--transcript"], [])   # query transcript; materialized index


class Pairs:
    """For each kind of output: a process using cells, and a reference."""

    def __init__(self, d, flags):
        self.p = [(Replay(d, flags + k), Replay(d, ["-fparallel=off",
                                                    "--no-cells"] + k))
                  for k in KINDS]

    def build(self):
        """None, or (kind, cells output, reference output) that differ"""
        bad = None
        for k, (a, b) in zip(KINDS, self.p):
            x, y = a.build(), b.build()
            if x != y and not bad:
                bad = (" ".join(k) or "json", x, y)
        return bad

    def stats(self):
        return [a.stats() for a, _ in self.p]

    def close(self):
        for a, b in self.p:
            a.close()
            b.close()


def directed(d):
    fails = 0
    for name, versions in DIRECTED:
        for fn in FILES:
            with open(os.path.join(d, fn), "w") as f:
                f.write("")
        pairs = Pairs(d, ["-fparallel=on", "-fparallel-threads=1",
                          "-fparallel-chunk=1"])
        for k, files in enumerate(versions):
            for fn, text in files.items():
                with open(os.path.join(d, fn), "w") as f:
                    f.write(text)
            bad = pairs.build()
            if bad:
                print("FAIL directed '%s' build %d (%s)" % (name, k, bad[0]))
                fails += 1
                break
        else:
            # the point of the cache: the last build reused something, and
            # an unchanged one everything
            want = EXPECT[name]
            for (reused, stored), _ in pairs.stats():
                if ((want == "reuse" and reused < 1) or
                        (want == "all" and (stored or not reused)) or
                        (want == "recompute" and stored < 1)):
                    print("FAIL directed '%s': %d cells reused, %d stored"
                          % (name, reused, stored))
                    fails += 1
                    break
        pairs.close()
    return fails


def main():
    r = random.Random(SEED)
    d = tempfile.mkdtemp()
    fails = directed(d)
    reused = stored = 0
    for it in range(N):
        gen_program(r, d)
        t = r.choice([1, 2, 4])
        c = r.choice([1, 16, 64, 200])
        pairs = Pairs(d, ["-fparallel=on", "-fparallel-threads=%d" % t,
                          "-fparallel-chunk=%d" % c])
        history = []
        for step in range(STEPS):
            if step:
                history.append(edit(r, d))
            bad = pairs.build()
            if bad:
                what, a, b = bad
                keep = os.path.join(d, "fail%d" % it)
                os.makedirs(keep, exist_ok=True)
                for fn in FILES:
                    with open(os.path.join(d, fn)) as x, \
                            open(os.path.join(keep, fn), "w") as y:
                        y.write(x.read())
                with open(os.path.join(keep, "cells.out"), "wb") as x:
                    x.write(a)
                with open(os.path.join(keep, "ref.out"), "wb") as x:
                    x.write(b)
                print("FAIL program %d step %d (%s; threads=%d chunk=%d, "
                      "edits %s): %s" % (it, step, what, t, c, history, keep))
                fails += 1
                break
        for _, (ru, st) in pairs.stats():
            reused += ru
            stored += st
        pairs.close()
    print("%d programs x %d builds, %d failures; cells %d reused, %d stored"
          % (N, STEPS, fails, reused, stored))
    return 1 if fails else 0


sys.exit(main())
