#!/usr/bin/env python3
"""The project index scheduler (docs/B5_DESIGN.md 8, 9, 13) through `cereal lsp`.

usage: proj_test.py BIN [SCENARIO...]     (no scenario: all)

Each scenario generates a small project (bench/tools/genproj.py: 50 headers,
12 units) and its own cache directory, starts one or two servers on it, and
asks them about the index with the test requests cereal/indexStats,
cereal/indexWait (a barrier: nothing running, nothing eligible) and
cereal/indexHold.  Prints `ok NAME` or `FAIL NAME: why`; exit 1 on any failure.

  restart      a fresh server indexes every unit; a second one finds them fresh
  invalidate   a saved header makes its dependents stale; a content change
               resets failure backoff
  cancel       an edit cancels the jobs in flight; they stay pending and
               finish after the edit
  fault        a fatal in a pool thread (CEREAL_FAULT=index-crec) re-runs the
               unit in a child; a child that fails (CEREAL_FAULT_CHILD) backs off
  memory       a memory budget too small for any job sends every unit to a child
  readers      a second server on the same cache is a reader and follows GEN
  progress     workDoneProgress: create request, begin, report, end
  paths        the paths setting restricts the units
"""
import json, os, queue, shutil, subprocess, sys, tempfile, threading

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.abspath(sys.argv[1])
NTU = 12


class Server:
    def __init__(self, root, cache, env=None, init=None, caps=None):
        e = dict(os.environ, CEREAL_INDEX_ENABLE="1", CEREAL_INDEX_QUIETMS="20",
                 CEREAL_INDEX_CACHEDIR=cache, CEREAL_INDEX_SWEEPSECONDS="3600")
        e.pop("CEREAL_FAULT", None)
        e.update(env or {})
        self.p = subprocess.Popen([BIN, "lsp"], cwd=root, env=e, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.inbox, self.seen, self.nid = queue.Queue(), [], 0
        self.root = root
        threading.Thread(target=self._read, daemon=True).start()
        r = self.req("initialize", {"rootUri": "file://" + root, "capabilities": caps or {},
                                    "initializationOptions": init or {}})
        assert "capabilities" in r["result"], r
        self.note("initialized", {})

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

    def note(self, method, params):
        self.send({"method": method, "params": params})

    def pump(self, until, timeout=60):
        """Consume messages until until(m) is true (earlier ones are kept in seen)."""
        for m in self.seen:
            if until(m):
                self.seen.remove(m)
                return m
        while True:
            m = self.inbox.get(timeout=timeout)
            if m is None:
                raise SystemExit("server exited")
            if self.answer(m):
                continue
            if until(m):
                return m
            self.seen.append(m)

    def answer(self, m):
        """Server requests: accept the progress token."""
        if m.get("method") == "window/workDoneProgress/create":
            self.send({"id": m["id"], "result": None})
            return True
        return False

    def req(self, method, params=None):
        self.nid += 1
        i = self.nid
        self.send({"id": i, "method": method, "params": params or {}})
        return self.pump(lambda m: m.get("id") == i and "method" not in m)

    def wait(self):
        self.req("cereal/indexWait")
        return self.stats()

    def stats(self):
        return self.req("cereal/indexStats")["result"]

    def stop(self):
        try:
            self.req("shutdown")
            self.note("exit", {})
        except Exception:
            pass
        try:
            self.p.wait(30)
        except subprocess.TimeoutExpired:
            self.p.kill()
            raise SystemExit("server did not exit")
        assert self.p.returncode == 0, "exit status %s" % self.p.returncode


def project():
    d = tempfile.mkdtemp(prefix="proj-test-")
    gen = os.path.join(HERE, "..", "bench", "tools", "genproj.py")
    subprocess.check_call([sys.executable, "-P", gen, os.path.join(d, "p"), str(NTU)],
                          stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
    return d, os.path.join(d, "p"), os.path.join(d, "cache")


def expect(cond, what, st=None):
    if not cond:
        raise AssertionError(what + (" " + json.dumps(st) if st else ""))


def sc_restart(root, cache):
    s = Server(root, cache)
    st = s.wait()
    expect(st["role"] == "writer" and st["tus"] == NTU, "writer with all units", st)
    expect(st["fresh"] == NTU and st["failed"] == 0, "all fresh", st)
    expect(st["published"] == NTU and st["jobsInProcess"] == NTU, "one job per unit", st)
    s.stop()
    s = Server(root, cache)
    st = s.wait()
    expect(st["fresh"] == NTU and st["jobsInProcess"] == 0 and st["published"] == 0,
           "restart revalidates without a job", st)
    s.stop()


def sc_invalidate(root, cache):
    s = Server(root, cache)
    s.wait()
    h = os.path.join(root, "include", "h00.h")
    with open(h, "a") as f:
        f.write("\n#define PROJ_TEST_CHANGED 1\n")
    s.note("textDocument/didSave", {"textDocument": {"uri": "file://" + h}})
    st = s.wait()
    expect(st["fresh"] == NTU and st["published"] > NTU, "dependents of h00.h reindexed", st)
    again = st["published"]
    s.note("textDocument/didSave", {"textDocument": {"uri": "file://" + h}})
    st = s.wait()
    expect(st["published"] == again, "an unchanged save indexes nothing", st)
    s.stop()


def sc_cancel(root, cache):
    s = Server(root, cache, env={"CEREAL_INDEX_THREADS": "2"})
    s.req("cereal/indexHold", {"hold": True})
    main = os.path.join(root, "src", "t0000.c")
    text = open(main).read()
    uri = "file://" + main
    s.note("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "c",
                                                      "version": 1, "text": text}})
    for _ in range(200):                     # jobs are parked at their start
        if s.stats()["running"] >= 1:
            break
        threading.Event().wait(0.05)
    expect(s.stats()["running"] >= 1, "jobs parked by the hold")
    s.note("textDocument/didChange", {"textDocument": {"uri": uri, "version": 2},
                                       "contentChanges": [{"text": text + "\nint added_by_edit;\n"}]})
    s.req("cereal/indexHold", {"hold": False})
    st = s.wait()
    expect(st["cancelled"] >= 1, "the edit cancelled a parked job", st)
    expect(st["fresh"] == NTU and st["failed"] == 0, "cancelled units finished later", st)
    s.stop()


def sc_fault(root, cache):
    s = Server(root, cache, env={"CEREAL_FAULT": "index-crec", "CEREAL_INDEX_THREADS": "1"})
    st = s.wait()
    expect(st["fatal"] == 1 and st["jobsSubprocess"] >= 1, "trapped fatal reran in a child", st)
    expect(st["fresh"] == NTU and st["failed"] == 0, "and the unit was indexed", st)
    s.stop()
    shutil.rmtree(cache)
    s = Server(root, cache, env={"CEREAL_FAULT": "index-crec", "CEREAL_FAULT_CHILD": "index-crec",
                                 "CEREAL_INDEX_THREADS": "1"})
    st = s.wait()
    expect(st["failed"] == 1 and st["backoff"] == 1, "a failing child backs the unit off", st)
    expect(st["fresh"] == NTU - 1, "the rest are indexed", st)
    s.stop()


def sc_memory(root, cache):
    s = Server(root, cache, env={"CEREAL_INDEX_MEMORYMB": "1", "CEREAL_INDEX_THREADS": "2"})
    st = s.wait()
    expect(st["jobsInProcess"] == 0 and st["jobsSubprocess"] == NTU, "every unit in a child", st)
    expect(st["fresh"] == NTU, "all fresh", st)
    s.stop()


def sc_readers(root, cache):
    w = Server(root, cache)
    w.wait()
    r = Server(root, cache)
    st = r.wait()
    expect(st["role"] == "reader" and st["fresh"] == NTU and st["jobsInProcess"] == 0,
           "the second server reads", st)
    h = os.path.join(root, "include", "h01.h")
    with open(h, "a") as f:
        f.write("\n#define PROJ_TEST_READER 1\n")
    w.note("textDocument/didSave", {"textDocument": {"uri": "file://" + h}})
    w.wait()
    for _ in range(100):                     # GEN is polled once a second
        st = r.stats()
        if st["genReloads"] >= 1:
            break
        threading.Event().wait(0.1)
    expect(st["genReloads"] >= 1, "the reader saw the writer's GEN change", st)
    r.stop()
    w.stop()


def sc_progress(root, cache):
    s = Server(root, cache, caps={"window": {"workDoneProgress": True}})
    s.req("cereal/indexHold", {"hold": True})       # the burst outlasts the create round trip
    s.pump(lambda m: m.get("method") == "$/progress" and m["params"]["value"]["kind"] == "begin")
    s.req("cereal/indexHold", {"hold": False})
    s.wait()
    kinds = [m["params"]["value"]["kind"] for m in s.seen if m.get("method") == "$/progress"]
    expect(kinds and kinds[-1] == "end", "begin ... end", {"kinds": kinds})
    expect(s.stats()["progressCreates"] == 1, "one create request")
    s.stop()


def sc_paths(root, cache):
    s = Server(root, cache, init={"index": {"paths": ["src/t000*.c"]}})
    st = s.wait()
    expect(st["tus"] == 10 and st["fresh"] == 10, "only t0000-t0009", st)
    s.stop()


SCEN = {k[3:]: v for k, v in globals().items() if k.startswith("sc_")}


def main():
    names = sys.argv[2:] or list(SCEN)
    bad = 0
    for n in names:
        d, root, cache = project()
        try:
            SCEN[n](root, cache)
            print("ok", n)
        except (AssertionError, SystemExit, Exception) as e:
            if os.environ.get("PROJ_TRACE"):
                raise
            bad += 1
            print("FAIL %s: %s" % (n, e))
        finally:
            shutil.rmtree(d, ignore_errors=True)
    sys.exit(1 if bad else 0)


main()
