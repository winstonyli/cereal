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
  wsym         workspace/symbol finds names of unopened files (a static, a macro),
               keeps a definition and not its declaration, drops what the disk
               no longer matches until the index catches up, and maps a name of
               an open document through the edit
  xref         references, definition, declaration and call hierarchy answer from the
               other units' records, mapped through an open document's edit
  rename       rename across units (B5_DESIGN.md 11.5): one edit per use in every file,
               as documentChanges (the open document's version, null for the others) or
               changes; refused for a name used in a macro body (R4, R7), for an incomplete
               index (R2), for too many units (R5), for a new name that collides (R6); and
               ContentModified when an edited file changes during the checks
  takeover     a reader becomes the writer when the writer exits
  wpos         workspace/symbol positions in a CR LF file with a character outside the BMP
  spin         with the sweep deadline passed and jobs running, the server uses no CPU
  evict        an idle project directory is evicted, one in use is kept
  private      memory-only indexing uses a private root named for the process; a dead one's is removed
  config       a touched compdb is dropped, a changed command reindexes its unit, a
               half-written one is not taken and no manifest is removed for it
  corrupt      a damaged record is rejected online and its units are re-indexed
"""
import json, os, queue, shutil, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.abspath(sys.argv[1])
NTU = 12


class Server:
    def __init__(self, root, cache, env=None, init=None, caps=None, settle=True):
        e = dict(os.environ, CEREAL_INDEX_ENABLE="1", CEREAL_INDEX_QUIETMS="20",
                 CEREAL_INDEX_CACHEDIR=cache, CEREAL_INDEX_SWEEPSECONDS="3600")
        e.pop("CEREAL_FAULT", None)
        e.update(env or {})
        self.p = subprocess.Popen([BIN, "lsp"], cwd=root, env=e, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.inbox, self.seen, self.nid = queue.Queue(), [], 0
        self.root = root
        self.settle = settle    # a didOpen waits for its check (a request waits only 1.5 s for it)
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
        if method == "textDocument/didOpen" and self.settle:
            self.req("cereal/waitIdle")

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

    def ws(self, query):
        return self.req("workspace/symbol", {"query": query})["result"]

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
    s = Server(root, cache, env={"CEREAL_INDEX_THREADS": "2"}, settle=False)
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
    p0 = w.stats()["published"]
    w.note("textDocument/didSave", {"textDocument": {"uri": "file://" + h}})
    changed = w.wait()["published"] - p0
    expect(0 < changed, "the save reindexed its dependents", w.stats())
    for _ in range(100):                     # GEN is polled once a second
        st = r.stats()
        if st["genReloads"] >= 1 and st["manReloads"] >= changed:
            break
        threading.Event().wait(0.1)
    expect(st["genReloads"] >= 1, "the reader saw the writer's GEN change", st)
    expect(st["manReloads"] == changed, "and reloaded only the manifests that changed", st)
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


def where(path, name):
    """(line, character) of the first occurrence of name in the file."""
    text = open(path, newline="").read()
    i = text.index(name)
    return text.count("\n", 0, i), i - (text.rfind("\n", 0, i) + 1)


def spots(res, path):
    return [(r["location"]["range"]["start"]["line"], r["location"]["range"]["start"]["character"])
            for r in res if r["location"]["uri"] == "file://" + path]


def poll(what, f, tries=200):
    for _ in range(tries):
        v = f()
        if v:
            return v
        threading.Event().wait(0.05)
    raise AssertionError(what)


def sc_wsym(root, cache):
    c = os.path.join(root, "src", "t0000.c")
    h = os.path.join(root, "include", "h00.h")
    s = Server(root, cache, settle=False)    # the edit is mapped before the check finishes
    s.wait()
    expect(spots(s.ws("tu0_count"), c) == [where(c, "tu0_count")], "a static of an unopened unit", s.ws("tu0_count"))
    expect(spots(s.ws("M00_MAX"), h) == [where(h, "M00_MAX")], "a macro of an unopened header")
    r = s.ws("h24_f4")
    expect(len(r) == 1 and spots(r, c) == [where(c, "h24_f4")], "the definition, not the declaration", r)
    # the file changes behind the index's back: its names are dropped, then come back
    text = open(c, newline="").read()
    s.req("cereal/indexHold", {"hold": True})
    with open(c, "w", newline="") as f:
        f.write("/* moved */\n" + text)
    expect(not s.ws("tu0_count"), "a file the index did not read is not trusted")
    s.req("cereal/indexHold", {"hold": False})
    s.note("textDocument/didSave", {"textDocument": {"uri": "file://" + c}})
    expect(poll("back", lambda: spots(s.ws("tu0_count"), c) == [where(c, "tu0_count")], 600),
           "and the index catches up", s.ws("tu0_count"))
    s.wait()
    # an open document that differs from its disk: the name is mapped through the edit
    uri = "file://" + c
    text = open(c, newline="").read()
    s.note("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "c", "version": 1,
                                                      "text": text}})
    s.note("textDocument/didChange", {"textDocument": {"uri": uri, "version": 2},
                                       "contentChanges": [{"text": "/* one\n two */\n" + text}]})
    line, ch = where(c, "tu0_count")
    got = s.ws("tu0_count")
    expect(spots(got, c) == [(line + 2, ch)], "mapped through the edit, once", got)
    s.stop()


def sc_xref(root, cache):
    # references, definition and call hierarchy across units (B5_DESIGN.md 11.2-11.4):
    # h01_f2 is declared in h01.h, defined in t0008.c, called in t0000, t0003, t0005
    t0, t3, t5, t8 = [os.path.join(root, "src", "t%04d.c" % i) for i in (0, 3, 5, 8)]
    h = os.path.join(root, "include", "h01.h")
    s = Server(root, cache)
    s.wait()

    def open_doc(path, text, version=1):
        s.note("textDocument/didOpen", {"textDocument": {"uri": "file://" + path, "languageId": "c",
                                                          "version": version, "text": text}})

    def at(path, line, ch):
        return {"textDocument": {"uri": "file://" + path}, "position": {"line": line, "character": ch}}

    def locs(res):
        return sorted((r["uri"], r["range"]["start"]["line"], r["range"]["start"]["character"])
                      for r in res)
    text = open(t0, newline="").read()
    open_doc(t0, text)
    line, ch = where(t0, "h01_f2(")
    ctx = dict(at(t0, line, ch), context={"includeDeclaration": True})
    got = locs(s.req("textDocument/references", ctx)["result"])
    dl, dc = where(t8, "int h01_f2(")
    want = sorted([("file://" + p,) + where(p, "h01_f2(") for p in (t0, h, t3, t5)] +
                  [("file://" + t8, dl, dc + 4)])
    expect(got == want, "references: the open unit, the header, the definition and other units",
           {"got": got, "want": want})
    ctx["context"]["includeDeclaration"] = False
    got = locs(s.req("textDocument/references", ctx)["result"])
    expect(len(got) == len(want) - 2 and all(g[0] != "file://" + h for g in got),
           "without includeDeclaration: uses only", got)
    # the definition of a function the open unit only declares is in the other unit
    got = s.req("textDocument/definition", at(t0, line, ch))["result"]
    expect(locs(got) == [("file://" + t8, dl, dc + 4)], "definition: the defining unit", got)
    got = s.req("textDocument/declaration", at(t0, line, ch))["result"]
    expect(locs(got) == [("file://" + h,) + where(h, "h01_f2(")], "declaration: the header", got)
    # an edit above the use: the open unit's places move, the others' do not.  The check of the
    # edit is held, so the request is answered from the carried index (a request waits only 1.5 s
    # for a check, and a slow one, under TSan and load, made this test race it)
    s.req("cereal/holdChecks", {"hold": True})
    s.note("textDocument/didChange", {"textDocument": {"uri": "file://" + t0, "version": 2},
                                       "contentChanges": [{"text": "/* one\n two */\n" + text}]})
    ctx = dict(at(t0, line + 2, ch), context={"includeDeclaration": False})
    got = locs(s.req("textDocument/references", ctx)["result"])
    s.req("cereal/holdChecks", {})
    s.req("cereal/waitIdle")
    mine = [g for g in got if g[0] == "file://" + t0]
    expect(mine == [("file://" + t0, line + 2, ch)] and len(got) == len(want) - 2,
           "references: the open document's places are mapped through the edit, once", got)
    # call hierarchy: h00_f2 is defined in t0008 and called from t0003, t0004 and t0008
    open_doc(t3, open(t3, newline="").read())
    l3, c3 = where(t3, "h00_f2(")
    item = s.req("textDocument/prepareCallHierarchy", at(t3, l3, c3))["result"]
    expect(item and item[0]["name"] == "h00_f2", "prepare", item)
    inc = s.req("callHierarchy/incomingCalls", {"item": item[0]})["result"]
    files = sorted({os.path.basename(c["from"]["uri"]) for c in inc})
    expect({"t0003.c", "t0004.c", "t0008.c"} <= set(files), "incoming calls across units", files)
    sites = sum(len(c["fromRanges"]) for c in inc)
    ranges = [(c["from"]["uri"], r["start"]["line"], r["start"]["character"])
              for c in inc for r in c["fromRanges"]]
    expect(len(ranges) == len(set(ranges)), "each call site once (the open unit and its record)", ranges)
    uses = s.req("textDocument/references", dict(at(t3, l3, c3), context={"includeDeclaration": False}))["result"]
    expect(sites == len(uses) and sites >= 4, "the sites are the uses", {"sites": sites, "uses": len(uses)})
    # the open unit's caller, and its outgoing call to h00_f2: shown at the definition (t0008),
    # not at the header's declaration
    mine3 = [c["from"] for c in inc if c["from"]["uri"] == "file://" + t3]
    expect(mine3, "an incoming caller in the open unit", inc)
    o3 = s.req("callHierarchy/outgoingCalls", {"item": mine3[0]})["result"]
    callee = [c["to"] for c in o3 if c["to"]["name"] == "h00_f2"]
    expect(callee and callee[0]["uri"] == "file://" + t8, "outgoing: the callee at its definition", o3)
    out = s.req("callHierarchy/outgoingCalls", {"item": item[0]})["result"]
    expect(out and all(c["to"]["name"] != "h00_f2" for c in out) and
           any(c["fromRanges"] for c in out),
           "outgoing calls of a function the open unit only declares: its definition's callees", out)
    s.stop()


def sc_rename(root, cache):
    # h01_f2 is declared in h01.h, defined in t0008.c, called in t0000, t0003, t0005;
    # h03_f4 is also spelled in the body of a macro (mb.h), invoked in t0000.c
    t0, t3, t5, t8 = [os.path.join(root, "src", "t%04d.c" % i) for i in (0, 3, 5, 8)]
    h = os.path.join(root, "include", "h01.h")
    with open(os.path.join(root, "include", "mb.h"), "w") as f:
        f.write("#define MB_CALL(p, i, s) h03_f4((p), (i), (s))\n")
    with open(t0, newline="") as f:
        text = f.read()
    text = text.replace('#include "h01.h"', '#include "h01.h"\n#include "mb.h"', 1)
    text += "\nint mb_use(s03_0_t *p, const char *s)\n{\n    return MB_CALL(p, 0, s);\n}\n"
    with open(t0, "w", newline="") as f:
        f.write(text)
    caps = {"workspace": {"workspaceEdit": {"documentChanges": True}}}
    s = Server(root, cache, caps=caps)
    s.wait()

    def at(path, line, ch):
        return {"textDocument": {"uri": "file://" + path}, "position": {"line": line, "character": ch}}

    def rename(path, name, new):
        line, ch = where(path, name)
        return s.req("textDocument/rename", dict(at(path, line, ch), newName=new))

    def places(res):
        return sorted((r["uri"], r["range"]["start"]["line"], r["range"]["start"]["character"])
                      for r in res)
    s.note("textDocument/didOpen", {"textDocument": {"uri": "file://" + t0, "languageId": "c",
                                                      "version": 7, "text": text}})
    line, ch = where(t0, "h01_f2(")
    refs = places(s.req("textDocument/references", dict(at(t0, line, ch),
                        context={"includeDeclaration": True}))["result"])
    r = s.req("textDocument/prepareRename", at(t0, line, ch))
    expect(r.get("result") and r["result"]["placeholder"] == "h01_f2", "prepareRename", r)
    r = rename(t0, "h01_f2(", "h01_renamed")
    expect("result" in r and r["result"], "the rename goes through", r)
    dc = r["result"]["documentChanges"]
    got = places([dict(uri=d["textDocument"]["uri"], range=e["range"]) for d in dc for e in d["edits"]])
    expect(got == refs and len(got) >= 5, "one edit for every use, in every file", {"got": got, "refs": refs})
    vers = {os.path.basename(d["textDocument"]["uri"]): d["textDocument"]["version"] for d in dc}
    expect(vers.get("t0000.c") == 7 and vers.get("h01.h") is None and vers.get("t0008.c") is None,
           "the open document's version, null for the others", vers)
    expect(all(e["newText"] == "h01_renamed" for d in dc for e in d["edits"]), "the new name")
    # the same for a client without documentChanges
    s2 = Server(root, cache + "2", env={"CEREAL_INDEX_CACHEDIR": cache + "2"})
    s2.wait()
    s2.note("textDocument/didOpen", {"textDocument": {"uri": "file://" + t0, "languageId": "c",
                                                       "version": 1, "text": text}})
    r = s2.req("textDocument/rename", dict(at(t0, line, ch), newName="h01_renamed"))
    ch2 = r["result"]["changes"]
    expect(places([dict(uri=u, range=e["range"]) for u, es in ch2.items() for e in es]) == refs,
           "changes: the same places", r)
    s2.stop()
    # a name that collides (R6) is refused by the gate
    r = rename(t0, "h01_f2(", "h01_f3")
    expect("error" in r and r["error"]["code"] == -32803, "a colliding name", r)
    # R4, R7: h03_f4 is spelled in a macro body
    line3, ch3 = where(t0, "h03_f4(")
    r = s.req("textDocument/prepareRename", at(t0, line3, ch3))
    expect("error" in r and "#define body" in r["error"]["message"], "prepareRename: macro body", r)
    r = rename(t0, "h03_f4(", "h03_renamed")
    expect("error" in r and "#define body" in r["error"]["message"] and "mb.h" in r["error"]["message"],
           "rename: macro body", r)
    # R2: a unit that is not indexed could use the name
    s.req("cereal/indexHold", {"hold": True})
    h7 = os.path.join(root, "include", "h07.h")
    with open(h7, "a") as f:
        f.write("/* touched */\n")
    s.note("textDocument/didSave", {"textDocument": {"uri": "file://" + h7}})
    poll("stale", lambda: s.stats()["file"] > 0)
    r = rename(t0, "h01_f2(", "h01_renamed")
    expect("error" in r and "incomplete" in r["error"]["message"], "R2: the index is incomplete", r)
    s.req("cereal/indexHold", {"hold": False})
    s.wait()
    r = rename(t0, "h01_f2(", "h01_renamed")
    expect("result" in r and r["result"], "and complete again", r)
    s.stop()
    # R5: at most renameMaxTUs units
    s = Server(root, cache, init={"index": {"renameMaxTUs": 1}})
    s.wait()
    s.note("textDocument/didOpen", {"textDocument": {"uri": "file://" + t0, "languageId": "c",
                                                      "version": 1, "text": text}})
    r = rename(t0, "h01_f2(", "h01_renamed")
    expect("error" in r and "renameMaxTUs" in r["error"]["message"], "R5: too many units", r)
    s.stop()
    # ContentModified: an edited file changes while the units are checked
    s = Server(root, cache)
    s.wait()
    s.note("textDocument/didOpen", {"textDocument": {"uri": "file://" + t0, "languageId": "c",
                                                      "version": 1, "text": text}})
    s.req("cereal/holdChecks", {"gate": True})     # the rename waits after its checks
    line, ch = where(t0, "h01_f2(")
    s.nid += 1
    rid = s.nid
    s.send({"id": rid, "method": "textDocument/rename",
            "params": dict(at(t0, line, ch), newName="h01_renamed", workDoneToken="rn")})
    s.pump(lambda m: m.get("method") == "$/progress" and m["params"]["value"]["kind"] == "end")
    with open(h, "a") as f:
        f.write("\n")
    s.req("cereal/holdChecks", {})
    r = s.pump(lambda m: m.get("id") == rid and "method" not in m)
    expect("error" in r and r["error"]["code"] == -32801, "ContentModified", r)
    s.stop()
    # $/cancelRequest: a rename cancelled after its checks answers RequestCancelled and writes
    # nothing; an id that is not pending is ignored; the same rename then goes through
    s = Server(root, cache)
    s.wait()
    s.note("textDocument/didOpen", {"textDocument": {"uri": "file://" + t0, "languageId": "c",
                                                      "version": 1, "text": text}})
    s.req("cereal/holdChecks", {"gate": True})
    s.nid += 1
    rid = s.nid
    s.send({"id": rid, "method": "textDocument/rename",
            "params": dict(at(t0, line, ch), newName="h01_renamed", workDoneToken="rn")})
    s.pump(lambda m: m.get("method") == "$/progress" and m["params"]["value"]["kind"] == "end")
    s.note("$/cancelRequest", {"id": rid + 1000})
    s.note("$/cancelRequest", {"id": rid})
    s.req("cereal/holdChecks", {})
    r = s.pump(lambda m: m.get("id") == rid and "method" not in m)
    expect("error" in r and r["error"]["code"] == -32800, "RequestCancelled", r)
    s.note("$/cancelRequest", {"id": rid})        # already answered: nothing happens
    r = rename(t0, "h01_f2(", "h01_renamed")
    expect("result" in r and r["result"], "the same rename goes through after a cancel", r)
    s.stop()
    # a request waits 1.5 s for the open unit's check, then answers with what there is: a rename
    # is refused ("not ready; retry"), never run on a missing check (Round 233: a slow check under
    # TSan and load made proj_test see this; didOpen now settles, and this pins the bound)
    s = Server(root, cache, caps=caps)
    s.wait()
    s.note("textDocument/didOpen", {"textDocument": {"uri": "file://" + t0, "languageId": "c",
                                                      "version": 1, "text": text}})
    s.req("cereal/holdChecks", {"hold": True})
    s.note("textDocument/didChange", {"textDocument": {"uri": "file://" + t0, "version": 2},
                                       "contentChanges": [{"text": text + "\n"}]})
    r = rename(t0, "h01_f2(", "h01_renamed")
    expect("error" in r and r["error"]["code"] == -32803 and "retry" in r["error"]["message"],
           "a rename meeting a pending check is refused, not run", r)
    s.req("cereal/holdChecks", {})
    s.req("cereal/waitIdle")
    r = rename(t0, "h01_f2(", "h01_renamed")
    expect("result" in r and r["result"], "the same rename goes through once the check is done", r)
    s.stop()
    db = os.path.join(root, "compile_commands.json")

    def add_unit(name, body):
        path = os.path.join(root, "src", name)
        open(path, "w").write(body)
        cc = json.load(open(db))
        e = dict(cc[0], file=path)
        e["arguments"] = [a if a != cc[0]["file"] else path for a in cc[0]["arguments"]]
        json.dump(cc + [e], open(db, "w"))
        return path
    # R7 on a declaration: another unit defines the function through a macro (the name is
    # formed by ##), so the edit list would miss it
    open(os.path.join(root, "include", "zm.h"), "w").write("int zm_fn(void);\n")
    add_unit("zm.c", '#include "../include/zm.h"\n#define PROTO(n) int n##_fn(void);\nPROTO(zm)\n'
                     'int zm_fn(void) { return 1; }\n')
    zu = add_unit("zu.c", '#include "../include/zm.h"\nint zu_use(void) { return zm_fn(); }\n')
    # F5: an opaque tag (forward declaration in a header) is one entity with its definition
    # in another unit
    open(os.path.join(root, "include", "zt.h"), "w").write("struct zt_opq;\nstruct zt_opq *zt_new(void);\n")
    add_unit("zt1.c", '#include "../include/zt.h"\nstruct zt_opq { int v; };\n'
                      'struct zt_opq *zt_new(void) { static struct zt_opq o; return &o; }\n')
    zt2 = add_unit("zt2.c", '#include "../include/zt.h"\nstruct zt_opq *zt_get(void) { return zt_new(); }\n')
    s = Server(root, cache)
    s.wait()
    ztext = open(zu).read()
    s.note("textDocument/didOpen", {"textDocument": {"uri": "file://" + zu, "languageId": "c",
                                                      "version": 1, "text": ztext}})
    r = rename(zu, "zm_fn()", "zm_renamed")
    # (the macro-formed declaration has no record entry today, so the gate's re-check of
    # zm.c is what refuses; F7 in B5_DESIGN)
    expect("error" in r and "zm.c" in r["error"]["message"],
           "a declaration formed by a macro in another unit is refused", r)
    zt2_text = open(zt2).read()
    s.note("textDocument/didOpen", {"textDocument": {"uri": "file://" + zt2, "languageId": "c",
                                                      "version": 1, "text": zt2_text}})
    zl, zc = where(zt2, "zt_opq")
    refs = s.req("textDocument/references", {"textDocument": {"uri": "file://" + zt2},
                 "position": {"line": zl, "character": zc}, "context": {"includeDeclaration": True}})["result"]
    got = sorted({os.path.basename(x["uri"]) for x in refs})
    expect(got == ["zt.h", "zt1.c", "zt2.c"], "F5: an opaque tag and its definition are one entity", got)
    s.stop()
    # F2: the new name is another external function of the project (defined in a unit that
    # does not read the edited files); an external with no definition cannot be renamed
    open(os.path.join(root, "include", "ex.h"), "w").write("int ex_f(void);\nint ex_lib(void);\n")
    add_unit("ex1.c", '#include "../include/ex.h"\nint ex_f(void) { return 1; }\n')
    ex2 = add_unit("ex2.c", '#include "../include/ex.h"\nint ex_use(void) { return ex_f() + ex_lib(); }\n')
    add_unit("ex3.c", 'int ex_g(void) { return 2; }\n')
    caps = {"workspace": {"workspaceEdit": {"documentChanges": True}}}
    # F3: a unit that #includes another unit's main file sees its static
    uni_a = add_unit("uni_a.c", 'static int uni_s(void) { return 1; }\nint uni_use(void) { return uni_s(); }\n')
    uni = add_unit("unity.c", '#include "uni_a.c"\nint uni_more(void) { return uni_s(); }\n')
    s = Server(root, cache, caps=caps)
    s.wait()
    for p in (ex2, uni_a):
        s.note("textDocument/didOpen", {"textDocument": {"uri": "file://" + p, "languageId": "c",
                                                          "version": 1, "text": open(p).read()}})
    r = rename(ex2, "ex_f()", "ex_g")
    m = r["error"]["message"] if "error" in r else ""
    expect("ex_g" in m and "link time" in m, "F2: a clash with another external function", r)
    r = rename(ex2, "ex_lib()", "ex_lib2")
    m = r["error"]["message"] if "error" in r else ""
    expect("no definition" in m, "F2: an external with no definition in the project", r)
    r = rename(ex2, "ex_f()", "ex_fine")
    expect("result" in r and r["result"], "F2: a fresh name is renamed", r)
    r = rename(uni_a, "uni_s(void)", "uni_t")
    got = sorted({os.path.basename(d["textDocument"]["uri"]): 0 for d in r["result"]["documentChanges"]}) \
        if "result" in r and r["result"] else r
    expect(got == ["uni_a.c", "unity.c"], "F3: a static is renamed in the unit that includes its file", got)
    n = sum(len(d["edits"]) for d in r["result"]["documentChanges"])
    expect(n == 3, "F3: the declaration and both uses, no more", n)
    s.stop()
    # F8: a name with more uses than the cap: references are cut and the log says so, a
    # rename is refused
    open(os.path.join(root, "include", "big.h"), "w").write("int bigf(void);\n")
    add_unit("big.c", '#include "../include/big.h"\nint bigf(void) { return 0; }\nvoid big_calls(void)\n{\n'
                      + "    bigf();\n" * 20100 + "}\n")
    bigu = add_unit("bigu.c", '#include "../include/big.h"\nint bigu(void) { return bigf(); }\n')
    s = Server(root, cache, caps=caps)
    s.wait()
    s.note("textDocument/didOpen", {"textDocument": {"uri": "file://" + bigu, "languageId": "c",
                                                      "version": 1, "text": open(bigu).read()}})
    bl, bc = where(bigu, "bigf()")
    refs = s.req("textDocument/references", dict(at(bigu, bl, bc), context={"includeDeclaration": True}))
    expect(len(refs["result"]) >= 20000 and len(refs["result"]) < 20010, "F8: references are cut at the cap",
           len(refs["result"]))
    s.pump(lambda m: m.get("method") == "window/logMessage" and "cut at" in m["params"]["message"])
    r = rename(bigu, "bigf()", "bigf_renamed")
    expect("error" in r and "more than" in r["error"]["message"], "F8: a rename of that size is refused", r)
    s.stop()
    # R2: an erroneous unit that reads the header blocks the rename; the message names it
    # and says how to exclude it
    add_unit("zbad.c", '#include "../include/h01.h"\nint zbad(void) { return zbad_undeclared; }\n')
    s = Server(root, cache)
    s.wait()
    s.note("textDocument/didOpen", {"textDocument": {"uri": "file://" + t0, "languageId": "c",
                                                      "version": 1, "text": text}})
    r = rename(t0, "h01_f2(", "h01_renamed")
    m = r["error"]["message"] if "error" in r else ""
    expect("zbad.c" in m and "error" in m and "cereal.index.paths" in m, "R2: an erroneous unit is named", r)
    s.stop()


def sc_wpos(root, cache):
    # a result's position in a file with CR LF line ends and a character outside the
    # BMP before it (two UTF-16 units), asked of a file big enough to need a line table
    c = os.path.join(root, "src", "t0000.c")
    text = open(c, newline="").read()
    text = "/* \U0001F600 */\r\n" + text.replace("static int tu0_count;", "/*\U0001F600*/ static int tu0_count;")
    text = text.replace("\r\n", "\n").replace("\n", "\r\n")
    open(c, "w", newline="", encoding="utf-8").write(text)
    s = Server(root, cache)
    s.wait()
    line = text.split("\r\n").index("/*\U0001F600*/ static int tu0_count;")
    want = (line, len("/*\U0001F600*/ static int ".encode("utf-16-le")) // 2)
    got = spots(s.ws("tu0_count"), c)
    expect(got == [want], "line and UTF-16 column in a CR LF file", {"got": got, "want": want})
    s.stop()


def cpu_seconds(pid):
    """utime + stime of a process, all its threads (/proc/PID/stat fields 14, 15)."""
    f = open("/proc/%d/stat" % pid).read().rsplit(")", 1)[1].split()
    return (int(f[11]) + int(f[12])) / os.sysconf("SC_CLK_TCK")


def sc_spin(root, cache):
    # with sweepSeconds expired and jobs running, the scheduler must sleep, not spin
    s = Server(root, cache, env={"CEREAL_INDEX_SWEEPSECONDS": "1", "CEREAL_INDEX_THREADS": "2"})
    s.req("cereal/indexHold", {"hold": True})
    poll("jobs parked", lambda: s.stats()["running"] >= 1)
    threading.Event().wait(1.5)                       # past the first sweep deadline
    t0, w0 = cpu_seconds(s.p.pid), time.time()
    threading.Event().wait(2.0)
    used = cpu_seconds(s.p.pid) - t0
    expect(used < 0.5, "the scheduler idles while jobs run (%.2f cpu-s in %.1f s)" %
           (used, time.time() - w0))
    s.req("cereal/indexHold", {"hold": False})
    s.wait()
    s.stop()


def sc_corrupt(root, cache):
    s = Server(root, cache)
    s.wait()
    s.stop()
    recs = [os.path.join(d, f) for d, _, fs in os.walk(cache) for f in fs if f.endswith(".rec")]
    expect(recs, "the first run wrote records")
    with open(recs[0], "ab") as f:                    # damaged on disk, manifests intact
        f.write(b"x")
    s = Server(root, cache)
    st = s.wait()
    expect(st["published"] >= 1 and st["fresh"] == NTU and st["lost"] == 0,
           "a damaged record is found online and its units re-indexed", st)
    s.stop()
    s = Server(root, cache)
    st = s.wait()
    expect(st["published"] == 0 and st["fresh"] == NTU, "and the store is whole again", st)
    s.stop()


def sc_config(root, cache):
    db = os.path.join(root, "compile_commands.json")
    s = Server(root, cache)
    st = s.wait()
    p0 = st["published"]

    def saved():
        s.note("textDocument/didSave", {"textDocument": {"uri": "file://" + db}})
        return s.wait()
    now = time.time() + 5
    os.utime(db, (now, now))                          # touched, not changed
    st = saved()
    expect(st["configReloads"] == 0 and st["published"] == p0, "a touched compdb is dropped", st)
    cc = json.load(open(db))
    cc[0]["arguments"].insert(1, "-DPROJ_TEST_NEW=1")
    json.dump(cc, open(db, "w"))
    st = saved()
    expect(st["configReloads"] == 1 and st["fresh"] == NTU and st["published"] == p0 + 1,
           "a changed command reindexes that unit only", st)
    open(db, "w").write("[]")                         # half written: the units stay
    st = saved()
    expect(st["configReloads"] == 1 and st["tus"] == NTU, "an empty compdb is not taken", st)
    tus = [f for d, _, fs in os.walk(cache) for f in fs if f.endswith(".tu")]
    expect(len(tus) == NTU + 1, "the replaced unit's manifest is left to the GC", {"tus": len(tus)})
    s.stop()


def sc_evict(root, cache):
    # eviction never takes a directory another server has open, and takes an idle one
    b = Server(root, cache)
    b.wait()
    root2 = root + "2"                                # another project, another directory
    shutil.copytree(root, root2)
    idle = os.path.join(cache, "index", "00000000deadbeef")
    os.makedirs(os.path.join(idle, "rec", "00"))
    for n in ("meta", "USE", "rec/00/x.rec"):
        open(os.path.join(idle, n), "w").write("x" * 4096)
        os.utime(os.path.join(idle, n), (1, 1))
    a = Server(root2, cache, init={"index": {"cacheMaxMB": 0.0001}})
    st = a.wait()
    expect(not os.path.exists(idle), "the idle directory was evicted", st)
    expect(st["evicted"] >= 1, "and counted", st)
    live = [d for d in os.listdir(os.path.join(cache, "index")) if d != "00000000deadbeef"]
    expect(len(live) == 2, "the directories in use were kept (two projects)", live)
    a.stop()
    b.stop()


def sc_private(root, cache):
    # no usable cache: a private root named for the process, and a dead one's is removed
    tmp = os.path.join(os.path.dirname(cache), "tmp")
    os.makedirs(tmp)
    stale = os.path.join(tmp, "cereal-index-999999-AbCdEf")
    os.makedirs(os.path.join(stale, "index"))
    s = Server(root, cache, env={"TMPDIR": tmp, "CEREAL_INDEX_CACHEDIR": ""},
               init={"index": {"cacheDir": ""}})
    st = s.wait()
    mine = [d for d in os.listdir(tmp) if d.startswith("cereal-index-%d-" % s.p.pid)]
    expect(len(mine) == 1 and not os.path.exists(stale), "own root made, dead one's removed",
           {"left": os.listdir(tmp)})
    expect(st["fresh"] == NTU, "memory-only indexing works", st)
    s.stop()
    expect(os.listdir(tmp) == [], "and removed on exit", {"left": os.listdir(tmp)})


def sc_takeover(root, cache):
    w = Server(root, cache)
    w.wait()
    r = Server(root, cache, env={"CEREAL_INDEX_LOCKRETRYSECONDS": "1"})
    expect(r.wait()["role"] == "reader", "the second server reads")
    w.stop()
    st = poll("takeover", lambda: (lambda x: x if x["role"] == "writer" else None)(r.stats()), 400)
    h = os.path.join(root, "include", "h02.h")
    with open(h, "a") as f:
        f.write("\n#define PROJ_TEST_TAKEOVER 1\n")
    r.note("textDocument/didSave", {"textDocument": {"uri": "file://" + h}})
    st = r.wait()
    expect(st["published"] >= 1 and st["fresh"] == NTU, "the new writer indexes", st)
    r.stop()


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
