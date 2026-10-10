import subprocess, time, os, resource, sys
OLD = os.path.expanduser("~/cereal-old/cereal"); NEW = os.path.expanduser("~/cereal-t/cereal")
d = "/tmp/perfw"; os.makedirs(d, exist_ok=True)
# A: header-heavy
open(d + "/hdr.c", "w").write("#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <math.h>\n#include <signal.h>\nint main(void){char b[8];memcpy(b,b+1,2);return 0;}\n")
# B: many prototypes, redeclared, with array params and restrict calls
n = 20000
with open(d + "/protos.c", "w") as f:
    f.write("typedef unsigned long size_t;\n")
    for i in range(n):
        f.write(f"void p{i}(int n, int a[n], char *restrict d, const char *restrict s, int (*m)[4]);\n")
        f.write(f"void p{i}(int n, int a[n], char *restrict d, const char *restrict s, int (*m)[4]);\n")
    f.write("void use(char *p, char *q, int *a, int (*m)[4]) {\n")
    for i in range(n):
        f.write(f"  p{i}(3, a, p, q, m);\n")
    f.write("}\n")
# C: plain code, no arrays/restrict (checks overhead on the common path)
with open(d + "/plain.c", "w") as f:
    for i in range(n):
        f.write(f"int g{i}(int x, int y);\nint g{i}(int x, int y) {{ return x + y * {i}; }}\n")
def run(exe, fn):
    t = time.perf_counter()
    r = subprocess.run(["nice", exe, "-fsyntax-only", "-std=c99", "-Wall", fn], capture_output=True)
    return time.perf_counter() - t, len(r.stderr.splitlines())
for name in ["hdr.c", "protos.c", "plain.c"]:
    fn = d + "/" + name
    best = {}
    for k in range(15):
        for tag, exe in (("old", OLD), ("new", NEW)):
            t, nl = run(exe, fn)
            if tag not in best or t < best[tag][0]:
                best[tag] = (t, nl)
    print("%-9s old %.3fs (%d lines)  new %.3fs (%d lines)  %+.1f%%" % (name, best["old"][0], best["old"][1], best["new"][0], best["new"][1], 100 * (best["new"][0] / best["old"][0] - 1)))
