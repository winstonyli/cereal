#!/usr/bin/env python3
"""fuzz_nfc.py CEREAL [ROUNDS] [GCC]: identifiers made of random characters
from src/ucnorm.h (combining marks, Hangul, the pairs check_nfc rejects,
...), written as UTF-8 or UCNs; the -Wnormalized warnings of cereal must equal
gcc's at every level.  Run: python3 -P bench/tools/fuzz_nfc.py ./cereal 20 < /dev/null"""
import os
import random
import re
import subprocess
import sys
import tempfile

here = os.path.dirname(os.path.abspath(__file__))
hdr = open(os.path.join(here, "..", "..", "src", "ucnorm.h")).read()
rows = [(int(a, 16), int(b, 16), int(c), int(d)) for a, b, c, d in re.findall(
    r"\{0x([0-9a-f]+), 0x([0-9a-f]+), (\d+), (\d+)\},", hdr)]
pairs = [(int(a, 16), int(b, 16)) for a, b in re.findall(
    r"\{0x([0-9a-f]+), 0x([0-9a-f]+)\},", hdr[hdr.index("nfc_bad"):])]
cereal = sys.argv[1]
rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 20
gcc = sys.argv[3] if len(sys.argv) > 3 else "gcc-13"

pool = []
for lo, hi, comb, kind in rows:
    if hi - lo > 40:
        pool += [random.randint(lo, hi) for _ in range(3)]
    else:
        pool += list(range(lo, hi + 1))
pool = [c for c in pool if c >= 0xA0 and c not in range(0xD800, 0xE000)]
comb = [c for lo, hi, cb, k in rows if cb for c in range(lo, min(hi, lo + 3) + 1)]
hangul = list(range(0x1100, 0x1113)) + list(range(0x1161, 0x1176)) + \
    list(range(0x11A8, 0x11C3)) + [0xAC00, 0xAC01, 0xD7A3, 0xAC1C]


def ch(c, ucn):
    if ucn:
        return "\\u%04x" % c if c <= 0xFFFF else "\\U%08x" % c
    return chr(c)


def ident(rng):
    seq = []
    for _ in range(rng.randint(1, 4)):
        r = rng.random()
        if r < 0.25 and pairs:
            c, p = rng.choice(pairs)
            seq += [p, c]
        elif r < 0.45:
            seq.append(rng.choice(hangul))
        elif r < 0.6:
            seq.append(rng.choice(comb))
        elif r < 0.7:
            seq.append(ord(rng.choice("abcAEIOUxyz_9")))
        else:
            seq.append(rng.choice(pool))
    ucn = rng.random() < 0.5
    s = "".join(ch(c, ucn) if c >= 0x80 else chr(c) for c in seq)
    return s if not s[0].isdigit() else "x" + s


def warns(cmd, path):
    r = subprocess.run(cmd + [path], capture_output=True, text=True,
                       stdin=subprocess.DEVNULL, env=dict(os.environ, LC_ALL="C"))
    return [l.replace(path, "F") for l in (r.stdout + r.stderr).splitlines()
            if "Wnormalized" in l]


bad = 0
tmp = tempfile.mkdtemp()
for seed in range(rounds):
    rng = random.Random(seed)
    path = os.path.join(tmp, "f%d.c" % seed)
    with open(path, "w", encoding="utf-8") as f:
        for i in range(200):
            f.write("int %s;\n" % ident(rng))
    for lvl in ("", "-Wnormalized=nfkc", "-Wnormalized=id", "-Wnormalized=none"):
        for std, ped in (("-std=c99", "-pedantic"), ("-std=gnu99", "-pedantic"), ("-std=gnu99", "-Wall")):
            fl = ["-fsyntax-only", std, ped] + ([lvl] if lvl else [])
            a, b = warns([gcc] + fl, path), warns([cereal] + fl, path)
            if a != b:
                bad += 1
                print("seed %d %s %s: %d vs %d" % (seed, std, lvl, len(a), len(b)))
                for x in sorted(set(a) ^ set(b))[:4]:
                    print("   ", x)
print("mismatches:", bad)
sys.exit(1 if bad else 0)
