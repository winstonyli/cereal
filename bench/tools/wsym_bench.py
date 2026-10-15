#!/usr/bin/env python3
"""workspace/symbol latency on a large project (docs/B5_DESIGN.md 15, stop rule 3).

usage: wsym_bench.py BIN CORPUS_DIR [REPEAT] [--keep]

CORPUS_DIR holds what bench/fetch_corpus.sh unpacks (lupa, uvloop, zstandard).
Builds a compile database over libuv, Lua 5.1-5.5, LuaJIT 2.0/2.1, the zstd
amalgamation and cereal's own src, each unit under three flag sets (about 1,800
translation units), starts `cereal lsp` with indexing on and a private cache,
opens libuv's core.c, waits for the index, and times a mix of 22 queries
(REPEAT times each, default 15).  Prints the index time, the name count and the
per-query p50, then the overall p50, p95 and max.  --keep leaves the project
and cache in place (the directory is printed).  Python never reads stdin here.
"""
import glob, json, os, queue, shutil, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "..", "src"))
FLAG_SETS = [[], ["-DCFG_A=1"], ["-DNDEBUG", "-DCFG_B=2"]]
QUERIES = ["uv_", "uv_fs", "uv_tcp_connect", "lua_", "luaL_", "ZSTD_", "ZSTD_compress",
           "a", "buf", "qqq", "Z", "loop", "read", "stream", "parse", "cereal", "pp_",
           "xx", "str", "init", "close", "tcp"]


def units(corpus):
    """(directory, file, extra flags) of every unit."""
    out = []
    uv = os.path.join(corpus, "uvloop-0.22.1", "vendor", "libuv")
    uvf = ["-I" + uv + "/include", "-I" + uv + "/src", "-D_GNU_SOURCE"]
    for sub in ("src/unix", "src", "test"):
        for f in sorted(glob.glob(uv + "/" + sub + "/*.c")):
            out.append((uv, f, uvf))
    lupa = os.path.join(corpus, "lupa-2.8", "third-party")
    for d in ("lua51/src", "lua52", "lua53", "lua54", "lua55", "luajit20/src", "luajit21/src"):
        for f in sorted(glob.glob(lupa + "/" + d + "/*.c")):
            out.append((os.path.dirname(f), f, ["-I" + os.path.dirname(f)]))
    z = os.path.join(corpus, "zstandard-0.25.0", "zstd")
    out.append((z, z + "/zstd.c", ["-I" + z]))
    for f in sorted(glob.glob(SRC + "/*.c")):
        out.append((SRC, f, ["-I" + SRC]))
    return out


def make_db(path, corpus):
    cc = []
    for d, f, fl in units(corpus):
        for extra in FLAG_SETS:
            cc.append({"directory": d, "file": f,
                       "arguments": ["cc", "-c", "-std=gnu11"] + fl + extra + ["-o", "/dev/null", f]})
    cc = [e for e in cc if not e["file"].endswith("libuv/src/unix/core.c")]   # the open file
    with open(path, "w") as fh:
        json.dump(cc, fh)
    return cc


class Server:
    def __init__(self, bin_, root, cache):
        e = dict(os.environ, CEREAL_INDEX_ENABLE="1", CEREAL_INDEX_CACHEDIR=cache,
                 CEREAL_INDEX_THREADS="4", CEREAL_INDEX_SWEEPSECONDS="3600")
        self.p = subprocess.Popen([bin_, "lsp"], cwd=root, env=e, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.inbox, self.nid = queue.Queue(), 0
        threading.Thread(target=self._read, daemon=True).start()
        self.req("initialize", {"rootUri": "file://" + root, "capabilities": {}})
        self.send({"method": "initialized", "params": {}})

    def _read(self):
        f = self.p.stdout
        while True:
            hdr, line = {}, f.readline()
            if not line:
                self.inbox.put(None)
                return
            while line.strip():
                k, _, v = line.decode().partition(":")
                hdr[k.strip().lower()] = v.strip()
                line = f.readline()
            self.inbox.put(json.loads(f.read(int(hdr["content-length"]))))

    def send(self, msg):
        data = json.dumps(dict(msg, jsonrpc="2.0")).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(data) + data)
        self.p.stdin.flush()

    def req(self, method, params=None, timeout=600):
        self.nid += 1
        i = self.nid
        self.send({"id": i, "method": method, "params": params or {}})
        while True:
            m = self.inbox.get(timeout=timeout)
            if m is None:
                raise SystemExit("server exited")
            if m.get("method") == "window/workDoneProgress/create":
                self.send({"id": m["id"], "result": None})
            elif m.get("id") == i and "method" not in m:
                return m.get("result")

    def stop(self):
        try:
            self.req("shutdown", timeout=30)
            self.send({"method": "exit", "params": {}})
            self.p.wait(30)
        except Exception:
            self.p.kill()


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p * len(v)))]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    keep = "--keep" in sys.argv
    if len(args) < 2:
        sys.exit(__doc__)
    bin_, corpus = os.path.abspath(args[0]), os.path.abspath(args[1])
    repeat = int(args[2]) if len(args) > 2 else 15
    work = tempfile.mkdtemp(prefix="wsym-bench-")
    cache = os.path.join(work, "cache")
    db = os.path.join(work, "compile_commands.json")
    cc = make_db(db, corpus)
    open_file = os.path.join(corpus, "uvloop-0.22.1", "vendor", "libuv", "src", "unix", "core.c")
    print("%d translation units, workspace %s" % (len(cc), work))
    s = Server(bin_, work, cache)
    try:
        s.send({"method": "workspace/didChangeConfiguration", "params": {"settings": {}}})
        s.send({"method": "textDocument/didOpen", "params": {"textDocument": {
            "uri": "file://" + open_file, "languageId": "c", "version": 1,
            "text": open(open_file).read()}}})
        t0 = time.time()
        s.req("cereal/indexWait", timeout=1800)
        st = s.req("cereal/indexStats")
        print("index: %.1f s, %d units fresh, %d names, %d records" %
              (time.time() - t0, st["fresh"], st["names"], st["records"]))
        for q in QUERIES[:3]:                       # warm the pages
            s.req("workspace/symbol", {"query": q})
        per, allv = {}, []
        for _ in range(repeat):
            for q in QUERIES:
                t = time.perf_counter()
                r = s.req("workspace/symbol", {"query": q})
                ms = (time.perf_counter() - t) * 1000
                per.setdefault(q, []).append((ms, len(r)))
                allv.append(ms)
        for q in QUERIES:
            v = [x[0] for x in per[q]]
            print("  %-16s %3d results  p50 %6.1f ms  max %6.1f ms" %
                  (q, per[q][0][1], pct(v, 0.5), max(v)))
        p95 = pct(allv, 0.95)
        print("all %d queries: p50 %.1f ms, p95 %.1f ms, max %.1f ms" %
              (len(allv), pct(allv, 0.5), p95, max(allv)))
        limit = float(os.environ.get("WSYM_P95_MS", "100"))
        verdict = p95 <= limit
        print("%s: p95 %.1f ms against the %.0f ms limit (WSYM_P95_MS)" %
              ("ok" if verdict else "FAIL", p95, limit))
    finally:
        s.stop()
        if keep:
            print("kept", work)
        else:
            shutil.rmtree(work, ignore_errors=True)
    return 0 if verdict else 1


sys.exit(main())
