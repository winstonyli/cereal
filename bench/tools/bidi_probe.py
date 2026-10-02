#!/usr/bin/env python3
"""Run gcc-13 and cereal on small bidi snippets and print both outputs.

usage: bidi_probe.py CEREAL [-Wflags...]   (snippets are the table below)
"""
import subprocess, sys

CEREAL = sys.argv[1]
FLAGS = sys.argv[2:]
RLO, LRI, PDI, PDF = "\u202e", "\u2066", "\u2069", "\u202c"
TESTS = {
    "c1": "/*" + RLO + "abcdef*/\n",
    "c2": "/*abc" + RLO + "def*/\n",
    "c3": "/*abc" + RLO + "def" + LRI + "gh*/\n",
    "c4": "/*abc" + RLO + "d" + LRI + "e" + PDI + "gh*/\n",
    "c5": "/*abc" + RLO + "d" + PDF + "e*/\n",
    "c6": "/*abc" + PDF + "d*/\n",
    "s1": 'char*s="abc' + RLO + 'def";\n',
    "s2": 'char*s="abc' + RLO + "d" + LRI + 'ef";\n',
    "l1": "//abc" + RLO + "def\n",
    "i1": "int a" + RLO + "b;\n",
    "k1": "int c = 'a" + RLO + "';\n",
    "t": "int x;" + RLO + "\n",
    "m": "/*abc" + RLO + "d*/ /*e" + LRI + "*/\n",
    "u1": "char*s=\"\\u202e\";\n",
}


def run(cmd, src):
    open("/tmp/b.c", "w", encoding="utf-8").write(src)
    r = subprocess.run(cmd + ["/tmp/b.c"], capture_output=True, text=True,
                       encoding="utf-8")
    return [l.replace("\u2018", "'").replace("\u2019", "'")
            for l in r.stderr.splitlines()
            if "warning" in l or "error" in l]


for k, v in TESTS.items():
    base = ["-fsyntax-only", "-std=c99", "-pedantic", *FLAGS]
    g = run(["gcc-13", *base], v)
    c = run([CEREAL, *base], v)
    print(k, "OK" if g == c else "DIFF", "\n  g:", g, "\n  c:", c)
