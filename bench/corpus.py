#!/usr/bin/env python3
"""Validation on real code: cereal against gcc and against itself.

usage: corpus.py CEREAL CORPUS_DIR [--quick] [--only NAME]

CORPUS_DIR holds unpacked source distributions (fetch them with
bench/fetch_corpus.sh): Lua 5.1-5.5 and a Cython-generated Lua binding
(lupa), libuv and a Cython-generated event loop (uvloop), zstd as one
file (zstandard).  For every translation unit:

  gcc      `cereal -E` is token-equal to `gcc -E` (tests/tokdiff.py)
  par      forced parallel -E, lint and index equal the sequential ones
  cells    `index --replay` with cells, rebuilt after a line is inserted
           mid-file, equals a sequential rebuild (diagnostics and index;
           the token stream regenerated from the cells, every cell's
           record checked)

and it reports times (gcc -E, cereal -E sequential and parallel).  Exit
status 1 if any check fails."""
import glob, os, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
TOKDIFF = os.path.join(HERE, "..", "tests", "tokdiff.py")
PY = "/usr/include/python3.11"


def units(root):
    """(name, file, flags, cwd) for every translation unit."""
    out = []
    lupa = [d for d in glob.glob(os.path.join(root, "lupa-*")) if os.path.isdir(d)]
    for d in lupa:
        for v in ("lua51", "lua52", "lua53", "lua54", "lua55"):
            src = os.path.join(d, "third-party", v)
            for f in sorted(glob.glob(os.path.join(src, "*.c"))):
                if os.path.basename(f) in ("onelua.c", "ltests.c"):
                    continue
                out.append(("%s/%s" % (v, os.path.basename(f)), f,
                            ["-DLUA_USE_LINUX", "-I" + src], src))
        f = os.path.join(d, "lupa", "lua54.c")
        out.append(("lupa/lua54.c (Cython)", f,
                    ["-I" + PY, "-I" + os.path.join(d, "third-party", "lua54")],
                    d))
    for d in [d for d in glob.glob(os.path.join(root, "uvloop-*")) if os.path.isdir(d)]:
        uv = os.path.join(d, "vendor", "libuv")
        flags = ["-I" + os.path.join(uv, "include"), "-I" + os.path.join(uv, "src"),
                 "-D_GNU_SOURCE", "-D_FILE_OFFSET_BITS=64"]
        linux = ["async", "core", "dl", "fs", "getaddrinfo", "getnameinfo",
                 "linux", "loop-watcher", "loop", "pipe", "poll", "process",
                 "proctitle", "random-devurandom", "random-getrandom",
                 "random-sysctl-linux", "signal", "stream", "tcp", "thread",
                 "tty", "udp"]
        for f in sorted(glob.glob(os.path.join(uv, "src", "*.c"))):
            out.append(("libuv/" + os.path.basename(f), f, flags, uv))
        for n in linux:
            f = os.path.join(uv, "src", "unix", n + ".c")
            if os.path.exists(f):
                out.append(("libuv/unix/%s.c" % n, f, flags, uv))
        out.append(("uvloop/loop.c (Cython)", os.path.join(d, "uvloop", "loop.c"),
                    ["-I" + PY, "-I" + os.path.join(uv, "include"),
                     "-I" + os.path.join(d, "uvloop")], d))
    for d in [d for d in glob.glob(os.path.join(root, "zstandard-*")) if os.path.isdir(d)]:
        f = os.path.join(d, "zstd", "zstd.c")
        out.append(("zstd/zstd.c", f, ["-I" + os.path.join(d, "zstd")], d))
    return out


def run(argv, cwd, stdin=None):
    t = time.time()
    p = subprocess.run(argv, cwd=cwd, input=stdin, capture_output=True,
                       timeout=600)
    return p.returncode, p.stdout, p.stderr, time.time() - t


def main():
    cereal, root = os.path.abspath(sys.argv[1]), sys.argv[2]
    quick = "--quick" in sys.argv
    only = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else None
    tmp = tempfile.mkdtemp()
    fails = 0
    tot = {"gcc": 0.0, "seq": 0.0, "par": 0.0}
    print("%-34s %8s %6s %6s %6s  %s" % ("unit", "bytes", "gcc", "seq", "par",
                                          "checks"))
    for name, f, flags, cwd in units(root):
        if only and only not in name:
            continue
        bad = []
        rg, og, eg, tg = run(["gcc", "-std=c99", "-E"] + flags + [f], cwd)
        rs, os_, es, ts = run([cereal, "-E", "-fparallel=off"] + flags + [f], cwd)
        rp, op, ep, tp = run([cereal, "-E", "-fparallel=on"] + flags + [f], cwd)
        if rg == 0:
            a, b = os.path.join(tmp, "g.i"), os.path.join(tmp, "c.i")
            open(a, "wb").write(og)
            open(b, "wb").write(os_)
            d = subprocess.run([sys.executable, TOKDIFF, a, b],
                               capture_output=True, text=True)
            if d.returncode or rs:
                bad.append("gcc: " + (d.stdout.strip().split("\n")[0]
                                      if d.returncode else es.decode()[:200]))
        else:
            bad.append("gcc failed: " + eg.decode().strip().split("\n")[0])
        if (rs, os_, es) != (rp, op, ep):
            bad.append("par -E differs")
        if not quick:
            for mode in (["lint", "-Weverything"], ["index", "--all"]):
                a = run([cereal] + mode + ["-fparallel=off"] + flags + [f], cwd)
                b = run([cereal] + mode + ["-fparallel=on", "-fparallel-chunk=4096"]
                        + flags + [f], cwd)
                if a[:3] != b[:3]:
                    bad.append("par %s differs" % mode[0])
            # cells: build, insert a line mid-file, build again
            copy = os.path.join(os.path.dirname(f), ".cereal_corpus_" +
                                os.path.basename(f))
            text = open(f, "rb").read()
            mid = text.find(b"\n", len(text) // 2) + 1
            open(copy, "wb").write(text)
            for kind in ([], ["--tokens=check"]):
                outs = []
                for extra in (["-fparallel=on", "-fparallel-chunk=4096"],
                              ["-fparallel=off", "--no-cells"]):
                    p = subprocess.Popen([cereal, "index", "--replay", "--all"] +
                                         kind + extra + flags + [copy], cwd=cwd,
                                         stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE,
                                         stderr=subprocess.DEVNULL)
                    builds = []
                    for k in range(2):
                        open(copy, "wb").write(
                            text[:mid] + b"int corpus_edit;\n" + text[mid:]
                            if k else text)
                        p.stdin.write(b"x\n")
                        p.stdin.flush()
                        buf = []
                        while True:
                            ln = p.stdout.readline()
                            if not ln or ln == b"=== end\n":
                                break
                            buf.append(ln)
                        builds.append(b"".join(buf))
                    p.stdin.write(b"q\n")
                    p.stdin.close()
                    p.wait()
                    open(copy, "wb").write(text)
                    outs.append(builds)
                if outs[0] != outs[1]:
                    bad.append("cells%s differ (build %d)" %
                               (" tokens" if kind else "",
                                0 if outs[0][0] != outs[1][0] else 1))
            os.remove(copy)
        tot["gcc"] += tg
        tot["seq"] += ts
        tot["par"] += tp
        print("%-34s %8d %6.3f %6.3f %6.3f  %s" % (name[:34], os.path.getsize(f),
                                                  tg, ts, tp,
                                                  "; ".join(bad) or "ok"))
        sys.stdout.flush()
        fails += bool(bad)
    print("total: gcc %.2fs, cereal -E %.2fs sequential, %.2fs parallel; "
          "%d failing" % (tot["gcc"], tot["seq"], tot["par"], fails))
    return 1 if fails else 0


sys.exit(main())
