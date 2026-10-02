#!/usr/bin/env python3
"""Differential fuzz of -Wbidi-chars, stray characters and UTF-8 identifiers.

usage: bidi_fuzz.py CEREAL [N [SEED]]   (run where gcc-13 is installed)
Random lines (strings, character constants, comments, identifiers) built from
bidirectional controls in UTF-8 and UCN spellings are checked by gcc-13 and
cereal under every -Wbidi-chars= mode, with and without -pedantic; the
bidi/stray/extended-character diagnostics must agree (as sets of lines)."""
import random
import subprocess
import sys

CEREAL = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 300
random.seed(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
B = "\\"
CH = ["‪", "‫", "‬", "‭", "‮", "⁦", "⁧",
      "⁨", "⁩", "‎", "‏", "a", "b", " ", "é", "́",
      "中", "​", "\t", "€"]
UCN = ["u202a", "u202b", "u202c", "u202d", "u202e", "u2066", "u2067", "u2068",
       "u2069", "u200e", "u200f", "U0000202e", "U00002069", "u00e9", "u{202e}"]
MODES = [None, "any", "unpaired", "ucn", "unpaired,ucn", "any,ucn", "none",
         "none,ucn", "no"]


def piece(ucn_ok, ident=False):
    if ucn_ok and random.random() < 0.3:
        # cereal does not take the delimited \u{...} form in identifiers
        return B + random.choice([u for u in UCN if not (ident and "{" in u)])
    return random.choice(CH)


def body(ucn_ok):
    return "".join(piece(ucn_ok) for _ in range(random.randint(0, 6)))


def line():
    k = random.choice("sSclbi")
    if k == "s":
        return 'char*s%d="%s";' % (random.randint(0, 99), body(True))
    if k == "S":
        return "int c%d='%s';" % (random.randint(0, 99), body(True))
    if k == "c":
        return "// %s" % body(False)
    if k == "l":
        return "/* %s */" % body(False)
    if k == "b":
        return "/* %s\n%s */" % (body(False), body(False))
    return "int a%s%s;" % (random.choice(["", "b"]),
                           "".join(piece(True, True) for _ in range(random.randint(1, 3))))


def run(cmd, path):
    r = subprocess.run(cmd + [path], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    out = set()
    for l in r.stderr.splitlines():
        if ("bidi" in l or "stray" in l or "extended character" in l) and \
           ("warning" in l or "error" in l):
            l = l.replace("‘", "'").replace("’", "'")
            out.add(l.split(": ", 1)[0].split(":", 1)[-1] + ": " +
                    l.split(": ", 1)[1].replace("/tmp/bf.c", ""))
    return out


bad = 0
for n in range(N):
    src = "\n".join(line() for _ in range(random.randint(1, 4))) + "\n"
    open("/tmp/bf.c", "w", encoding="utf-8").write(src)
    for mode in MODES:
        for ped in ([], ["-pedantic"]):
            fl = ["-fsyntax-only", "-std=gnu99"] + ped
            if mode == "no":
                fl.append("-Wno-bidi-chars")
            elif mode:
                fl.append("-Wbidi-chars=" + mode)
            g, c = run(["gcc-13"] + fl, "/tmp/bf.c"), run([CEREAL] + fl, "/tmp/bf.c")
            if g != c:
                bad += 1
                if bad <= 8:
                    print("DIFF", fl, repr(src))
                    print("  gcc-only:", sorted(g - c))
                    print("  cereal-only:", sorted(c - g))
print("cases", N, "diffs", bad)
