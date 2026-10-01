import subprocess, sys, itertools, re, os, tempfile
# usage: apfuzz.py gcc|cereal [level]  -> prints "a || b => msgs"
POOL = ["int *p", "int p[]", "int p[0]", "int p[1]", "int p[2]", "int p[3]", "int p[static 2]", "int p[static 3]",
        "int p[const 2]", "int p[restrict 2]", "int p[restrict 3]", "int p[*]", "int p[n]", "int p[m]", "int p[n + 1]",
        "int p[f (0)]", "int p[f (1)]", "int p[2][3]", "int p[][3]", "int p[n][3]", "int p[n][m]", "int p[m][n]",
        "int p[*][3]", "int p[*][*]", "int p[3][m]", "int p[2][n]", "int (*p)[]", "int (*p)[2]", "int (*p)[n]",
        "int (*p)[*]", "int (*p)[m]", "int p[][n]", "int p[][*]", "int *p[2]", "int *p[3]", "short p[n]", "int (*p)[n][m]", "int (*p)[m][n]",
        "int (*p)[2][n]", "int (*p)[2][m]", "int (*p)[n][2]"]
tool = sys.argv[1]
lvl = sys.argv[2] if len(sys.argv) > 2 else "2"
exe = ["gcc-13"] if tool == "gcc" else [os.path.expanduser("~/cereal-t/cereal")]
opts = ["-fsyntax-only", "-Wall", "-Warray-parameter=" + lvl, "-Wvla-parameter"]
d = tempfile.mkdtemp()
res = {}
for a, b in itertools.product(POOL, POOL):
    src = "extern int f (int);\nvoid t (int n, int m, %s);\nvoid t (int n, int m, %s);\n" % (a, b)
    fn = os.path.join(d, "x.c")
    open(fn, "w").write(src)
    r = subprocess.run(exe + opts + [fn], capture_output=True, text=True)
    out = [l for l in r.stderr.replace("‘", "'").replace("’", "'").split("\n") if re.search(r"warning|error|note", l)]
    out = [re.sub(r"^.*?x\.c:", "", l) for l in out]
    res[(a, b)] = out
import json
json.dump({a + " || " + b: v for (a, b), v in res.items()}, open(sys.argv[3] if len(sys.argv) > 3 else "/tmp/fz_%s_%s.json" % (tool, lvl), "w"))
