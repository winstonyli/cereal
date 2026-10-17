#!/usr/bin/env python3
"""`enable` against the real preprocessor (docs/CONFIG_DESIGN.md section 7, the oracle).

usage: pc_enable.py CEREAL CORPUS_DIR [--only NAME] [--per-unit N] [--bad N] [--cover]

For every corpus unit runs `cereal lint` with CEREAL_PCDUMP="oracle enable": each
satisfiable group that is off on the host carries the command-line words
(-D/-U/-std) that would turn it on.  Per unit, N of those groups (evenly spaced,
default 8; only groups the fold knows exactly) are re-run with the words appended to the unit's flags, and the same
group must now be taken by the preprocessor (or, where the host valuation
cannot tell, at least be on in the fold: "unverified").  Prints, per family: groups with an
overlay, groups where `enable` says "none", groups checked, groups that became
active, groups still off, groups no longer found.  Exit status 1 if a checked
group is still off.

With --cover: for every unit, the (at most 8) overlays `cover` picks are re-run and
every group each claims must be taken (a group counts if any of its instances is);
prints eligible and covered group counts and the claims that failed."""
import collections, os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(HERE, "corpus.py")).read()
exec(compile(src.rsplit("sys.exit(main())", 1)[0], "corpus.py", "exec"))

WORKERS = 6
GROUP = re.compile(r"pc: (\S+:\d+ \w+) sat=(-?\d) host=(\d) exact=(\d) taint=(\d) .*?oracle=(\w+)")


def run(cereal, flags, f, cwd, dump):
    env = dict(os.environ, CEREAL_PCDUMP=dump)
    p = subprocess.run(["nice", "-n", "10", cereal, "lint", "-fparallel=off"]
                       + flags + [f], cwd=cwd, env=env, capture_output=True,
                       timeout=900)
    return [l for l in p.stderr.decode(errors="replace").split("\n")
            if l.startswith("pc:")]


def one(job):
    cereal, name, f, flags, cwd, per = job
    lines = run(cereal, flags, f, cwd, "oracle enable")
    overlays, none, key = {}, 0, None
    for l in lines:
        m = GROUP.match(l)
        if m:
            key = m.group(1) if m.group(4) == "1" else None   # exact groups only
        elif l.startswith("pc:   enable ") and key:
            w = l[len("pc:   enable "):].strip()
            if w == "none":
                none += 1
            elif key not in overlays:
                overlays[key] = w
    keys = sorted(overlays)
    step = max(1, len(keys) // per) if per else 1
    res = collections.Counter(overlays=len(overlays), none=none)
    bad = []
    for k in keys[::step][:per]:
        again = run(cereal, flags + overlays[k].split(), f, cwd, "oracle")
        seen = [GROUP.match(l) for l in again]
        seen = [m for m in seen if m and m.group(1) == k]
        res["checked"] += 1
        if not seen:
            res["gone"] += 1
        elif any(m.group(3) == "1" and m.group(6) == "ok" for m in seen):
            res["active"] += 1
        elif any(m.group(3) == "1" and m.group(6) == "unknown" for m in seen):
            res["unverified"] += 1      # the fold says on; the run cannot tell
        elif all(m.group(5) == "1" for m in seen):
            res["unverified"] += 1      # an include was not followed in the re-run
        else:
            res["still off"] += 1
            bad.append("%s %s :: %s" % (name, k, overlays[k]))
    return name, res, bad


def one_cover(job):
    cereal, name, f, flags, cwd, per = job
    lines = run(cereal, flags, f, cwd, "oracle cover")
    res = collections.Counter()
    bad = []
    for l in lines:
        m = re.match(r"pc: cover total eligible=(\d+) covered=(\d+) overlays=(\d+)", l)
        if m:
            res["eligible"] += int(m.group(1))
            res["covered"] += int(m.group(2))
            res["overlays"] += int(m.group(3))
            continue
        m = re.match(r"pc: cover\[\d+\] (.*?) :: (.*)", l)
        if not m:
            continue
        words, claims = m.group(1), m.group(2).split()
        again = run(cereal, flags + words.split(), f, cwd, "oracle")
        seen = collections.defaultdict(list)
        for x in again:
            g = GROUP.match(x)
            if g:
                seen[g.group(1)].append(g)
        for c in claims:
            file_line, kind = c.rsplit(":", 1)
            ms = seen.get(file_line + " " + kind, [])
            res["claimed"] += 1
            if any(g.group(3) == "1" for g in ms):
                res["held"] += 1
            elif ms and all(g.group(5) == "1" for g in ms):
                res["unverified"] += 1
            else:
                res["failed"] += 1
                bad.append("%s %s :: %s" % (name, c, words))
    return name, res, bad


def main_cover(jobs, nbad):
    agg = collections.defaultdict(collections.Counter)
    bad = []
    with ThreadPoolExecutor(WORKERS) as ex:
        for name, res, b in ex.map(one_cover, jobs):
            agg[name.split("/")[0] if "/" in name else name].update(res)
            bad += b
    rc = 0
    for fam, a in sorted(agg.items()):
        print("%s: %d eligible groups, %d covered by %d overlays; claimed %d: %d held, "
              "%d unverified, %d failed" % (fam, a["eligible"], a["covered"], a["overlays"],
                                            a["claimed"], a["held"], a["unverified"], a["failed"]))
        rc |= bool(a["failed"])
    for l in bad[:nbad]:
        print("FAIL " + l[:400])
    return rc


def main():
    cereal, root = os.path.abspath(sys.argv[1]), sys.argv[2]
    only = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else None
    per = int(sys.argv[sys.argv.index("--per-unit") + 1]) if "--per-unit" in sys.argv else 8
    nbad = int(sys.argv[sys.argv.index("--bad") + 1]) if "--bad" in sys.argv else 12
    jobs = [(cereal, n, f, fl, cw, per) for n, f, fl, cw in units(root)
            if not only or only in n]
    if "--cover" in sys.argv:
        return main_cover(jobs, nbad)
    agg = collections.defaultdict(collections.Counter)
    bad = []
    with ThreadPoolExecutor(WORKERS) as ex:
        for name, res, b in ex.map(one, jobs):
            agg[name.split("/")[0] if "/" in name else name].update(res)
            bad += b
    rc = 0
    for fam, a in sorted(agg.items()):
        print("%s: %d groups with an overlay, %d with none; checked %d: %d active, "
              "%d unverified, %d still off, %d gone" % (
                  fam, a["overlays"], a["none"], a["checked"], a["active"],
                  a["unverified"], a["still off"], a["gone"]))
        rc |= bool(a["still off"])
    for l in bad[:nbad]:
        print("OFF " + l[:400])
    return rc


sys.exit(main())
