#!/usr/bin/env python3
"""The presence-condition fold against the real preprocessor (docs/S1_PLAN.md).

usage: pc_oracle.py CEREAL CORPUS_DIR [--only NAME] [--bad N]

For every corpus unit (the list of bench/corpus.py) runs
`cereal lint` with CEREAL_PCDUMP="oracle size time check": each group's condition
evaluated under the host valuation must say what the preprocessor did.
Prints per family: groups, agreement on exact (no opaque atom) groups with
and without skipped includes before them, opaque groups, fold times, the
largest group BDD, units that went over the node cap; then the first N
disagreements.  Exit status 1 if an exact, untainted group disagrees."""
import collections, os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(HERE, "corpus.py")).read()
exec(compile(src.rsplit("sys.exit(main())", 1)[0], "corpus.py", "exec"))

WORKERS = 6


def family(name):
    return name.split("/")[0] if "/" in name else name


def one(job):
    cereal, name, f, flags, cwd = job
    env = dict(os.environ, CEREAL_PCDUMP="oracle size time check")
    p = subprocess.run(["nice", "-n", "10", cereal, "lint", "-fparallel=off"]
                       + flags + [f], cwd=cwd, env=env, capture_output=True,
                       timeout=900)
    lines = [l for l in p.stderr.decode(errors="replace").split("\n")
             if l.startswith("pc:")]
    return name, lines


def main():
    cereal, root = os.path.abspath(sys.argv[1]), sys.argv[2]
    only = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else None
    nbad = int(sys.argv[sys.argv.index("--bad") + 1]) if "--bad" in sys.argv else 12
    jobs = [(cereal, n, f, fl, cw) for n, f, fl, cw in units(root)
            if not only or only in n]
    agg = collections.defaultdict(collections.Counter)
    times = collections.defaultdict(list)
    bad = []
    with ThreadPoolExecutor(WORKERS) as ex:
        for name, lines in ex.map(one, jobs):
            fam = family(name)
            a = agg[fam]
            a["units"] += 1
            tot = [l for l in lines if l.startswith("pc: total")]
            if not tot:
                a["no-output"] += 1
                continue
            m = re.search(r"over=(\d)", tot[0])
            a["over"] += int(m.group(1))
            for l in lines:
                mt = re.match(r"pc: fold_ms=([\d.]+)", l)
                if mt:
                    times[fam].append(float(mt.group(1)))
                mg = re.search(r"groups=(\d+)", l) if l.startswith("pc: total") else None
                if mg:
                    a["groups"] += int(mg.group(1))
                    a["unresolved"] += int(re.search(r"unresolved=(\d+)", l).group(1))
                    a["computed"] += int(re.search(r"computed=(\d+)", l).group(1))
                    a["maxsize"] = max(a["maxsize"], int(re.search(r"maxsize=(\d+)", l).group(1)))
                    a["maxtable"] = max(a["maxtable"], int(re.search(r"table=(\d+)", l).group(1)))
                mc = re.match(r"pc: check rank1 ok=(\d+) bad=(\d+) skipped=(\d+)", l)
                if mc:
                    for nm, x in zip(("ok", "bad", "skipped"), mc.groups()):
                        a["rank1 " + nm] += int(x)
                if l.startswith("pc: oracle"):
                    mu = re.search(r"undecided=(\d+)", l)
                    a["undecided"] += int(mu.group(1)) if mu else 0
                    for k, v, b in re.findall(r"([\w-]+) ok=(\d+) bad=(\d+)", l):
                        a[k + " ok"] += int(v)
                        a[k + " bad"] += int(b)
                if "oracle=BAD" in l and len(bad) < 400:
                    bad.append((name, l))
    rc = 0
    for fam, a in sorted(agg.items()):
        t = sorted(times[fam])
        ex_ok = a["exact-untainted ok"] + a["exact-tainted ok"]
        ex_all = ex_ok + a["exact-untainted bad"] + a["exact-tainted bad"]
        decid = a["exact-untainted ok"] + a["exact-untainted bad"]
        print("%s: %d units (%d over cap, %d no output), %d groups, skipped "
              "includes: %d computed, %d unresolved" % (
                  fam, a["units"], a["over"], a["no-output"], a["groups"],
                  a["computed"], a["unresolved"]))
        print("  exact groups %d (%.0f%% of groups): agree %d/%d untainted, "
              "%d/%d after a skipped include; opaque groups agree %d/%d (+%d host-unknown)" % (
                  ex_all, 100.0 * ex_all / max(1, a["groups"]),
                  a["exact-untainted ok"], decid, a["exact-tainted ok"],
                  a["exact-tainted ok"] + a["exact-tainted bad"],
                  a["opaque-untainted ok"] + a["opaque-tainted ok"],
                  a["opaque-untainted ok"] + a["opaque-tainted ok"] +
                  a["opaque-untainted bad"] + a["opaque-tainted bad"], a["undecided"]))
        print("  rank 1 against enumeration: %d ok, %d bad, %d too large" % (
            a["rank1 ok"], a["rank1 bad"], a["rank1 skipped"]))
        if t:
            print("  fold ms: mean %.1f, median %.1f, max %.1f; largest group "
                  "BDD %d nodes, largest table %d" % (sum(t) / len(t),
                                                      t[len(t) // 2], t[-1],
                                                      a["maxsize"], a["maxtable"]))
        rc |= bool(a["exact-untainted bad"] or a["rank1 bad"])
    for name, l in bad[:nbad]:
        print("BAD %s: %s" % (name, l[:300]))
    return rc


sys.exit(main())
