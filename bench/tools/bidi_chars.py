#!/usr/bin/env python3
"""Which code points does gcc-13 -Wbidi-chars=any flag?  Prints the message
for each candidate inside a string literal."""
import subprocess

cands = [0x061C, 0x200B, 0x200C, 0x200D, 0x200E, 0x200F, 0x2028, 0x2029,
         0x202A, 0x202B, 0x202C, 0x202D, 0x202E, 0x2060, 0x2066, 0x2067,
         0x2068, 0x2069, 0xFEFF, 0x206A, 0x206F, 0x2065]
for cp in cands:
    open("/tmp/b.c", "w", encoding="utf-8").write('char*s="%s";\n' % chr(cp))
    r = subprocess.run(["gcc-13", "-fsyntax-only", "-Wbidi-chars=any",
                        "/tmp/b.c"], capture_output=True, text=True,
                       encoding="utf-8")
    print("%04X" % cp, [l.split("warning: ")[1] for l in r.stderr.splitlines()
                        if "warning" in l])
