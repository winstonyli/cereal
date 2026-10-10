"""ccdb.py DB: see ccdb.sh."""
import json, os, re, shlex, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
CEREAL = os.environ.get("CEREAL", os.path.join(HERE, "..", "..", "cereal"))
GCC = os.environ.get("CEREAL_GCC", "gcc-13")
OUT = os.environ.get("CCDB_OUT", "/tmp/ccdb.out")
JOBS = int(os.environ.get("CCDB_JOBS", "8"))
M_ONE = {"-M", "-MM", "-MD", "-MMD", "-MP", "-MG"}
M_ARG = {"-MF", "-MT", "-MQ"}
QUOTES = {0x2018: "'", 0x2019: "'", 0x60: "'", 0x22: "'"}


def norm(text):
    keep = [re.sub(r" \[-W[a-z=0-9-]*\]$", "", l.translate(QUOTES))
            for l in text.splitlines() if ": warning:" in l or ": error:" in l]
    return sorted(set(keep))


def entries(path):
    base = os.path.dirname(os.path.abspath(path))
    for e in json.load(open(path)):
        d = os.path.normpath(os.path.join(base, e.get("directory", ".")))
        args = e["arguments"] if "arguments" in e else shlex.split(e["command"])
        yield d, e["file"], os.path.normpath(os.path.join(d, e["file"])), args


def gcc_args(args):
    out, i = [], 1
    while i < len(args):
        a = args[i]
        if a == "-o" or a in M_ARG:
            i += 1
        elif a == "-c" or a in M_ONE or (a.startswith("-o") and len(a) > 2):
            pass
        else:
            out.append(a)
        i += 1
    return out + ["-fsyntax-only"]


def one(job):
    path, absf, ents = job
    env = dict(os.environ, LC_ALL="C")
    g_lines, g_bad = set(), False
    for d, f, args in ents:
        p = subprocess.run([GCC] + gcc_args(args), cwd=d, capture_output=True,
                           env=env, stdin=subprocess.DEVNULL)
        g_lines.update(norm(p.stderr.decode(errors="replace")))
        g_bad |= p.returncode != 0
    c = subprocess.run([CEREAL, "check", "--compile-commands", path, absf, "-j1"],
                       capture_output=True, env=env, stdin=subprocess.DEVNULL)
    c_lines = norm(c.stderr.decode(errors="replace"))
    c_bad = c.returncode == 1
    why = []
    if c.returncode not in (0, 1):
        why.append("cereal exit %d" % c.returncode)
    if g_bad != c_bad:
        why.append("verdict gcc=%s cereal=%s" % (g_bad, c_bad))
    if sorted(g_lines) != c_lines:
        why.append("diagnostics")
        why += ["< " + l[:200] for l in sorted(g_lines - set(c_lines))]
        why += ["> " + l[:200] for l in sorted(set(c_lines) - g_lines)]
    return absf, why


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: ccdb.py DB")
    path = os.path.abspath(sys.argv[1])
    byfile, n = {}, 0
    for d, f, absf, args in entries(path):
        if not absf.endswith(".c") and "-x" not in args:
            continue                    # not C
        n += 1
        byfile.setdefault(absf, []).append((d, f, args))
    with ThreadPoolExecutor(JOBS) as ex:
        res = list(ex.map(one, [(path, a, e) for a, e in byfile.items()]))
    bad = [r for r in res if r[1]]
    with open(OUT, "w") as o:
        for f, why in bad:
            o.write("== %s\n%s\n" % (f, "\n".join(why)))
    print("%s: %d entries, %d files, %d differ" % (path, n, len(byfile), len(bad)))
    t = time.time()
    p = subprocess.run([CEREAL, "check", "--compile-commands", path, "-j12"],
                       capture_output=True, stdin=subprocess.DEVNULL)
    last = p.stderr.decode(errors="replace").strip().splitlines()[-1:]
    print("full replay -j12: %.1fs exit %d %s" %
          (time.time() - t, p.returncode, last[0] if last else ""))


main()
