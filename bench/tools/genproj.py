"""genproj.py OUT [TUS=1000] [SEED=1]: deterministic synthetic C project.

Writes OUT/include/hNN.h (50 shared headers, a DAG so fan-in and transitive
includes occur), OUT/src/tNNNN.c and OUT/compile_commands.json (4 distinct -D
sets).  Used by B5_DESIGN 5.5 / 14.7 (index cache sizes).
Run: python3 -P bench/tools/genproj.py OUT [TUS] [SEED] </dev/null
"""
import json, os, random, sys

NH = 50
SYS = ["stdio.h", "stdlib.h", "string.h", "stdint.h", "stddef.h", "errno.h",
       "pthread.h", "unistd.h", "assert.h", "limits.h", "stdarg.h", "time.h",
       "ctype.h", "signal.h", "fcntl.h", "sys/types.h", "sys/stat.h"]
DSETS = [["-DMODE_A=1", "-DUSE_THREADS"],
         ["-DMODE_B=1", "-DUSE_THREADS", "-DDEBUG"],
         ["-DMODE_A=1", "-DNDEBUG", "-DUSE_LOG"],
         ["-DMODE_C=2", "-DUSE_LOG", "-DUSE_THREADS"]]
FIELD_T = ["int", "unsigned", "long", "size_t", "uint32_t", "uint64_t",
           "char *", "const char *", "double", "void *", "int16_t", "uint8_t"]


def header(i, rnd):
    deps = sorted(rnd.sample(range(i), min(i, rnd.randint(0, 3)))) if i else []
    o = [f"#ifndef H{i:02d}_H", f"#define H{i:02d}_H"]
    for s in ["errno.h", "stdlib.h", "stdint.h", "stddef.h", "pthread.h"] + rnd.sample(SYS, rnd.randint(0, 3)):
        if f"#include <{s}>" not in o:
            o.append(f"#include <{s}>")
    for d in deps:
        o.append(f'#include "h{d:02d}.h"')
    for k in range(rnd.randint(4, 10)):
        o.append(f"#define M{i:02d}_{k} ({rnd.randint(1, 4096)})")
    o.append(f"#define M{i:02d}_MAX(a, b) ((a) > (b) ? (a) : (b))")
    o.append(f"#define M{i:02d}_CHECK(x) do {{ if (!(x)) abort(); }} while (0)")
    o.append(f"enum e{i:02d} {{ E{i:02d}_A, E{i:02d}_B = 7, E{i:02d}_C, E{i:02d}_N }};")
    for s in range(rnd.randint(2, 4)):
        o.append(f"typedef struct s{i:02d}_{s} {{")
        o.append("    int f0;")
        for f in range(1, rnd.randint(3, 9)):
            o.append(f"    {rnd.choice(FIELD_T)} f{f};")
        if s and rnd.random() < 0.4:
            o.append(f"    struct s{i:02d}_{s-1} *link;")
        o.append(f"}} s{i:02d}_{s}_t;")
    o.append(f"#ifdef MODE_A\n#define H{i:02d}_FLAVOR 1\n#elif defined(MODE_B)\n"
             f"#define H{i:02d}_FLAVOR 2\n#else\n#define H{i:02d}_FLAVOR 3\n#endif")
    o.append("#ifdef USE_THREADS")
    o.append(f"extern pthread_mutex_t h{i:02d}_lock;")
    o.append(f"int h{i:02d}_locked_call(s{i:02d}_0_t *p);")
    o.append("#endif")
    o.append("#ifdef DEBUG")
    o.append(f"void h{i:02d}_dump(const s{i:02d}_0_t *p);")
    o.append("#endif")
    nf = rnd.randint(6, 14)
    for f in range(nf):
        o.append(f"int h{i:02d}_f{f}(s{i:02d}_0_t *p, int n, const char *s);")
    for f in range(rnd.randint(2, 4)):
        o.append(f"static inline int h{i:02d}_in{f}(int a, int b)\n{{\n"
                 f"    return M{i:02d}_MAX(a, b) + {rnd.randint(1, 99)} + H{i:02d}_FLAVOR;\n}}")
    for f in range(rnd.randint(1, 3)):
        o.append(f"static int h{i:02d}_st{f}(int v)\n{{\n"
                 f"    if (v < 0) {{ errno = EINVAL; return -1; }}\n"
                 f"    return v * {rnd.randint(2, 9)} + E{i:02d}_C;\n}}")
    o.append(f"#endif /* H{i:02d}_H */")
    return "\n".join(o) + "\n", deps, nf


def tu(n, hdrs, nfs, rnd):
    o = []
    for s in ["stdio.h", "string.h"] + rnd.sample(SYS, rnd.randint(2, 6)):
        if f"#include <{s}>" not in o:
            o.append(f"#include <{s}>")
    for h in hdrs:
        o.append(f'#include "h{h:02d}.h"')
    o.append(f"static int tu{n}_count;")
    nfun = rnd.randint(6, 14)
    names = []
    for f in range(nfun):
        h = rnd.choice(hdrs)
        g = rnd.randrange(nfs[h])
        if f % 3 == 0 and f"h{h:02d}_f{g}" not in [x[0] for x in names]:  # define a function declared in a shared header
            nm, lead = f"h{h:02d}_f{g}", ""
            o.append(f"int {nm}(s{h:02d}_0_t *p, int n, const char *s)")
        else:
            nm = f"t{n}_fn{f}"
            lead = "static " if f % 2 else ""
            o.append(f"{lead}int {nm}(s{h:02d}_0_t *p, int n, const char *s)")
        o.append("{\n    int acc = 0, i;")
        o.append(f"    M{h:02d}_CHECK(p != NULL);")
        o.append(f"    for (i = 0; i < n && i < M{h:02d}_0; i++) {{")
        for _ in range(rnd.randint(3, 9)):
            h2 = rnd.choice(hdrs)
            g2 = rnd.randrange(nfs[h2])
            c = rnd.randint(0, 3)
            if c == 0:
                o.append(f"        acc += h{h2:02d}_f{g2}((s{h2:02d}_0_t *)p, i, s);")
            elif c == 1:
                o.append(f"        acc += h{h2:02d}_in0(acc, i) + h{h2:02d}_st0(i);")
            elif c == 2:
                o.append(f"        if (s && strlen(s) > {rnd.randint(1, 30)}) acc ^= (int)strlen(s);")
            else:
                o.append(f"        p->f0 = ({'int'})acc; tu{n}_count++;")
        o.append("    }")
        o.append(f'    if (acc < 0) fprintf(stderr, "%d %s\\n", acc, s ? s : "");')
        o.append("    return acc;\n}\n")
        names.append((nm, h))
    o.append(f"int t{n}_main(int argc, char **argv)\n{{\n    int r = 0;\n"
             f"    char obj[256];\n    memset(obj, 0, sizeof obj);")
    for nm, h in names[:6]:
        o.append(f"    r += {nm}((s{h:02d}_0_t *)obj, argc, argv[0]);")
    o.append("    return r;\n}")
    return "\n".join(o) + "\n"


def main():
    out = os.path.abspath(sys.argv[1])
    ntu = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    rnd = random.Random(seed)
    os.makedirs(out + "/include", exist_ok=True)
    os.makedirs(out + "/src", exist_ok=True)
    deps, nfs = [], []
    for i in range(NH):
        text, d, nf = header(i, rnd)
        deps.append(d)
        nfs.append(nf)
        with open(f"{out}/include/h{i:02d}.h", "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    # skewed popularity: low-numbered headers are included by most TUs
    weights = [1.0 / (1 + i) ** 0.6 for i in range(NH)]
    cc = []
    for n in range(ntu):
        k = rnd.randint(5, 15)
        picks = set()
        while len(picks) < k:
            picks.add(rnd.choices(range(NH), weights)[0])
        picks = sorted(picks)
        # the first header's struct must be visible: include it directly
        hdrs = picks
        name = f"src/t{n:04d}.c"
        with open(f"{out}/{name}", "w", encoding="utf-8", newline="\n") as f:
            f.write(tu(n, hdrs, nfs, rnd))
        cc.append({"directory": out, "file": name,
                   "arguments": ["gcc", "-c", "-std=gnu11", "-Iinclude"]
                   + DSETS[n % len(DSETS)] + ["-o", f"build/t{n:04d}.o", name]})
    with open(out + "/compile_commands.json", "w", encoding="utf-8", newline="\n") as f:
        json.dump(cc, f, indent=1)
        f.write("\n")


main()
