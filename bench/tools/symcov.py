#!/usr/bin/env python3
"""symcov.py [CEREAL] [--gnu11] [--no-dg] [--no-corpus]   symbol-index coverage

Runs `cereal --verify-symbols` over the units of bench/corpus.py (CEREAL_CORPUS,
default ~/corpus) and the gcc.dg and c-c++-common tests that pass par.py's
dg-options filter (CEREAL_GCCTS, default ~/gccts), each with its own flags
(file selection is par.py's: units() and dgjobs(), imported, not copied).
Prints one line (files, events, unindexed, excused), then every unindexed name
as file:line:col.  Exit 1 if any unindexed name, a failed index check or a crash.
--gnu11 replaces each file's -std= with gnu11 (the second pass of Round 188).
CEREAL_JOBS sizes the pool (at most 8); workers run at nice 10; no network.
Expect (Round 188): 4716 files, about 1.02 M events, 0 unindexed."""
import sys, os, re, subprocess, multiprocessing
S = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, S)
import par
SUM = re.compile(r'^symbols: (\d+) events, \d+ decls, \d+ files, \d+ bytes, (\d+) unindexed, (\d+) excused$')
UNI = re.compile(r"^(.*:\d+:\d+): unindexed '(.*)' \((.*)\)$")
def work(job):
    cer, f, flags, cwd, std = job
    try:
        p = subprocess.run([cer, "-fsyntax-only"] + std + flags + ["--verify-symbols", f], cwd=cwd,
                           capture_output=True, timeout=60, env=dict(os.environ, LC_ALL="C"),
                           stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return dict(f=f, bad=["timeout"])
    r = dict(f=f, ev=0, un=0, ex=0, names=[], bad=[])
    out = p.stdout.decode(errors="replace") + p.stderr.decode(errors="replace")
    seen = False
    for l in out.splitlines():
        m = SUM.match(l)
        if m: seen = True; r["ev"], r["un"], r["ex"] = map(int, m.groups())
        elif UNI.match(l): r["names"].append(l)
        elif l.startswith("verify:"): r["bad"].append(l)
    if not seen:
        if p.returncode == 1: r["skip"] = 1   # cereal rejected the command line (an option it lacks)
        else: r["bad"].append("no summary line (rc=%d)" % p.returncode)
    return r
def main():
    a = sys.argv[1:]
    gnu = "--gnu11" in a
    pos = [x for x in a if not x.startswith("--")]
    cer = os.path.abspath(pos[0] if pos else os.environ.get("CEREAL", os.path.join(S, "..", "..", "cereal")))
    jobs = []
    if "--no-corpus" not in a:
        root = os.environ.get("CEREAL_CORPUS", os.path.expanduser("~/corpus"))
        jobs += [(f, fl, cw, ["-std=c99", "-pedantic"]) for n, f, fl, cw in par.units(root)]
    if "--no-dg" not in a:
        os.environ["CEREAL_DGOPTS"] = "1"
        for d in ("gcc.dg", "c-c++-common"):
            os.environ["CEREAL_DGDIR"] = d
            jobs += par.dgjobs([])
    if gnu:
        jobs = [(f, fl, cw, [x for x in std if not x.startswith("-std=")] + ["-std=gnu11"]) for f, fl, cw, std in jobs]
    jobs = [(cer, f, fl, cw, std) for f, fl, cw, std in jobs]
    os.nice(10)
    n = max(1, min(8, int(os.environ.get("CEREAL_JOBS", "8"))))
    with multiprocessing.Pool(n) as p:
        res = list(p.imap(work, jobs, chunksize=4))
    ev = sum(r.get("ev", 0) for r in res); un = sum(r.get("un", 0) for r in res)
    ex = sum(r.get("ex", 0) for r in res)
    sk = sum(r.get("skip", 0) for r in res)
    bad = [(r["f"], b) for r in res for b in r["bad"]]
    print("files %d, events %d, unindexed %d, excused %d%s%s" % (len(res), ev, un, ex,
          ", %d skipped (rc 1, no index)" % sk if sk else "", ", %d FAILED" % len(bad) if bad else ""))
    for r in res:
        for l in r.get("names", []): print("unindexed:", l)
    for f, b in bad: print("failed: %s: %s" % (f, b))
    return 1 if un or bad else 0
if __name__ == "__main__":
    sys.exit(main())
