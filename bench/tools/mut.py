#!/usr/bin/env python3
"""Parser against gcc on mutated real code.

usage: parse_mutate.py CEREAL CORPUS_DIR [N_PER_UNIT] [SEED] [--only NAME]

Every translation unit of the corpus (bench/corpus.py) is preprocessed by
cereal; then, N_PER_UNIT times, one token of the preprocessed text is
deleted or duplicated, and both `gcc -std=c99 -fsyntax-only -x cpp-output`
and `cereal parse` judge the result.  gcc also checks meaning (types,
declarations), so where gcc rejects and cereal accepts, the mutation is a
semantic error cereal does not check yet: counted, not a failure.  A
failure is cereal rejecting what gcc accepts, or cereal crashing.  The
unmutated units must be accepted by both.  Exit status 1 on failures;
failing inputs are kept."""
import multiprocessing, os, random, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.argv, ARGS = sys.argv[:3], sys.argv  # corpus.py reads sys.argv at import
exec(open(os.path.join(HERE, "..", "corpus.py")).read().split("\ndef run(")[0])
sys.argv = ARGS

TOKEN = re.compile(r'''
    L?"(?:\\.|[^"\\\n])*" | L?'(?:\\.|[^'\\\n])*' |
    \.?[0-9](?:[eEpP][+-]|[0-9A-Za-z_.])* | [A-Za-z_$][A-Za-z0-9_$]* |
    \.\.\.|<<=|>>=|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||[*/%+\-&^|]=|\#\#|
    \S''', re.X)
MARKER = re.compile(r'^\s*#')


def spans(text):
    """(start, end) of every token outside linemarker/pragma lines."""
    out, pos = [], 0
    for line in text.splitlines(keepends=True):
        if not MARKER.match(line):
            for m in TOKEN.finditer(line):
                out.append((pos + m.start(), pos + m.end()))
        pos += len(line)
    return out


def judge(cereal, path):
    g = subprocess.run(["gcc", "-std=c99", "-pedantic", "-fsyntax-only", "-fpermissive", "-w",
                        "-x", "cpp-output", path], capture_output=True,
                       timeout=300)
    c = subprocess.run([cereal, "-fsyntax-only", "-std=c99", "-pedantic", path], capture_output=True,
                       timeout=300)
    ge = [l for l in g.stderr.decode(errors="replace").splitlines() if "error:" in l]
    res_g = g.returncode == 0
    judge.last = re.sub(r"'[^']*'", "'X'", ge[0].split("error:")[-1]) if ge else ""
    return res_g, c.returncode, c.stderr.decode(errors="replace")


def work(job):
    cereal, name, f, flags, cwd, n, seed, keep = job
    p = subprocess.run([cereal, "-E", "-fparallel=off"] + flags + [f],
                       cwd=cwd, capture_output=True, timeout=300)
    res = {"name": name, "fail": [], "semantic": 0, "agree": 0}
    if p.returncode:
        res["fail"].append("cereal -E failed")
        return res
    text = p.stdout.decode(errors="replace")
    toks = spans(text)
    r = random.Random("%s:%d" % (name, seed))
    with tempfile.TemporaryDirectory() as d:
        base = os.path.join(d, "u.i")
        with open(base, "w") as o:
            o.write(text)
        gok, crc, err = judge(cereal, base)
        if not gok or crc != 0:
            res["fail"].append("unmutated: gcc %s, cereal %d: %s" %
                               (gok, crc, err.strip().split("\n")[0]))
            return res
        for k in range(n):
            a, b = toks[r.randrange(len(toks))]
            how = r.choice(["delete", "duplicate"])
            mut = (text[:a] + text[b:] if how == "delete"
                   else text[:b] + " " + text[a:b] + text[b:])
            path = os.path.join(d, "m%d.i" % k)
            with open(path, "w") as o:
                o.write(mut)
            gok, crc, err = judge(cereal, path)
            if crc not in (0, 1):
                why = "cereal crashed (%d)" % crc
            elif crc == 1 and gok:
                why = "cereal rejects, gcc accepts: " + \
                    err.strip().split("\n")[0]
            else:
                if crc == 0 and not gok:
                    res["semantic"] += 1
                    res.setdefault("ai", []).append(judge.last)
                else:
                    res["agree"] += 1
                continue
            kept = os.path.join(keep, "%s.%d.i" % (re.sub(r"\W", "_", name), k))
            with open(kept, "w") as o:
                o.write(mut)
            res["fail"].append("%s token %r at %d: %s (%s)" %
                               (how, text[a:b], a, why, kept))
    return res


def main():
    cereal, root = os.path.abspath(ARGS[1]), ARGS[2]
    rest = [a for a in ARGS[3:] if not a.startswith("--")]
    n = int(rest[0]) if rest else 4
    seed = int(rest[1]) if len(rest) > 1 else 1
    only = ARGS[ARGS.index("--only") + 1] if "--only" in ARGS else None
    keep = tempfile.mkdtemp(prefix="parse_mutate.")
    jobs = [(cereal, name, f, flags, cwd, n, seed, keep)
            for name, f, flags, cwd in units(root)
            if not only or only in name]
    fails = agree = semantic = 0
    AI = {}
    with multiprocessing.Pool(os.cpu_count() * 3 // 4) as pool:
        for res in pool.imap(work, jobs):
            agree += res["agree"]
            for m in res.get("ai", []): AI[m] = AI.get(m, 0) + 1
            semantic += res["semantic"]
            for f in res["fail"]:
                print("FAIL %s: %s" % (res["name"], f))
                fails += 1
    for m, k in sorted(AI.items(), key=lambda kv: -kv[1])[:25]:
        print("  ACCEPTS-INVALID %4d %s" % (k, m))
    print("%d units x %d mutations: %d agree, %d semantic (gcc only), "
          "%d failures" % (len(jobs), n, agree, semantic, fails))
    return 1 if fails else 0


sys.exit(main())
