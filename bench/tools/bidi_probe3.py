#!/usr/bin/env python3
"""gcc-13 bidi warnings for mixed UTF-8/UCN contexts under every mode."""
import subprocess

B = "\\"
RLO, PDF, PDI = "‮", "‬", "⁩"
srcs = {
    "u8": 'char*s="%s";\n' % RLO,
    "ucn": 'char*s="%su202e";\n' % B,
    "u8,ucn": 'char*s="%s%su202e";\n' % (RLO, B),
    "ucn,u8": 'char*s="%su202e%s";\n' % (B, RLO),
    "u8 pdf": 'char*s="%s";\n' % PDF,
    "ucn pdf": 'char*s="%su202c";\n' % B,
    "u8,ucn,pdf": 'char*s="%s%su202e%s";\n' % (RLO, B, PDF),
    "ucnLRI u8 pdi": 'char*s="%su2066%s";\n' % (B, PDI),
    "ucn,u8 two pdf": 'char*s="%su202e%s%s";\n' % (B, RLO, PDF),
}
for k, v in srcs.items():
    open("/tmp/b.c", "w", encoding="utf-8").write(v)
    for fl in ("none", "unpaired", "any", "ucn", "none,ucn", "unpaired,ucn",
               "any,ucn"):
        r = subprocess.run(["gcc-13", "-fsyntax-only", "-Wbidi-chars=" + fl,
                            "/tmp/b.c"], capture_output=True, text=True,
                           encoding="utf-8")
        w = [l.split(": ", 1)[0].split(":", 1)[1] + " " +
             l.split(": warning: ")[1][:60].replace("‘", "").replace("’", "")
             for l in r.stderr.splitlines() if "warning" in l and "bidi" in l]
        print(k, "|", fl, "|", w)
