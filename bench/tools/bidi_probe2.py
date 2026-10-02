#!/usr/bin/env python3
"""Print gcc-13's bidi warnings for UCN-spelled contexts under each mode."""
import subprocess

B = "\\"        # one backslash
RLO, PDF = "‮", "‬"
srcs = {
    "ucn str": 'char*s="%su202e";\n' % B,
    "ucn str 2": 'char*s="%su202e%su202c";\n' % (B, B),
    "ucn mismatch": 'char*s="%s%su202c";\n' % (RLO, B),
    "ucn mismatch2": 'char*s="%su202e%s";\n' % (B, PDF),
    "ucn comment": "/* %su202e */\nint x;\n" % B,
    "ucn ident": "int a%su202eb;\n" % B,
    "ucn lone pdf": 'char*s="%su202c";\n' % B,
    "ucn char": "int c='%su202e';\n" % B,
    "ucn U": 'char*s="%sU0000202e";\n' % B,
}
for k, v in srcs.items():
    open("/tmp/b.c", "w", encoding="utf-8").write(v)
    for fl in ("unpaired", "ucn", "none,ucn", "unpaired,ucn", "any", "any,ucn"):
        r = subprocess.run(["gcc-13", "-fsyntax-only", "-Wbidi-chars=" + fl,
                            "/tmp/b.c"], capture_output=True, text=True,
                           encoding="utf-8")
        w = [l.split(": ", 1)[0].split(":", 1)[1] + " " +
             l.split(": warning: ")[1][:90].replace("‘", "").replace("’", "")
             for l in r.stderr.splitlines() if "warning" in l and "bidi" in l]
        print(k, "|", fl, "|", w)
