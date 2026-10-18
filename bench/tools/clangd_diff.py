#!/usr/bin/env python3
"""clangd_diff.py [CEREAL] [--no-real]   cereal lsp against clangd

Runs `cereal lsp` and `clangd` on the files of bench/clangd_diff/ (hand-written
cases from the B4 probes) and the real files REAL below (from CEREAL_CORPUS,
default ~/corpus; missing ones are skipped), each in a scratch workspace with
the same flags (-std=c11, compile_flags.txt for clangd, .cereal for cereal).
Requests, per file: inlayHint over the whole file (parameter hints only, kind
2); prepareCallHierarchy at every top-level function and variable that
clangd's documentSymbol lists, then incomingCalls and outgoingCalls of the
items each server returned.
Differences are normalized (names and call sites; details, ranges of items,
positions of items are dropped) and each is printed with the id of the
intended difference of docs/B4_DESIGN.md 10.2 it matches (C1, C3, C5, H1-H4,
H6, W1-W4; found by this script: C6, P1, see section 18), or UNEXPECTED.
workspace/symbol (B4c) is asked the QUERIES below of each server, with one
document open; a result is (name, kind, file, position of the name).
Project mode (docs/B5_DESIGN.md 14.6; --project=NAME for one project, --no-project to skip):
the units of PROJECTS (lua54, libuv's Linux units) get a compile_commands.json and both servers'
project indexes are completed first (cereal/indexWait; clangd's background index, its answers
the union of CLANGD_RUNS runs because one run's index is not deterministic).  Per file in
PROJECTS: references (includeDeclaration) of every top-level name clangd lists, definition and
declaration at up to 80 distinct calls.  Intended differences: P2 clangd lists the invocation of
a macro whose body uses the name at the macro call's own spelling (lua_call for lua_callk); where that
call sits in another macro's argument cereal presents it at the outermost invocation, as gcc
does (P6, the cereal side); P3 the
place a definition or declaration request answers (clangd: the header declaration, or the
declaration at a definition) when both answer; P4 cereal lists a use spelled in a macro
argument that clangd omits; P5 the request is in an inactive preprocessor region (cereal has
nothing there).  A miss is a clangd result in none of those classes (and, for definition and
declaration, a clangd answer where cereal has none); the stop rule is 2% of clangd's results.
Exit 1 on any unexpected difference or a failure to run.  Not in run.sh:
clangd may be absent.
No fixed sleeps: it waits for each server's index of the file by polling."""
import json, os, queue, re, shutil, subprocess, sys, tempfile, threading, time

S = os.path.dirname(os.path.abspath(__file__))
CORP = os.path.join(S, "..", "clangd_diff")
# (file, project root, include dirs relative to it, other flags): macro-heavy
# Lua, K&R-era-style libuv (function-pointer tables)
REAL = [("lupa-2.8/third-party/lua54/lstrlib.c", "lupa-2.8/third-party/lua54", ["."], []),
        ("lupa-2.8/third-party/lua54/lparser.c", "lupa-2.8/third-party/lua54", ["."], []),
        ("uvloop-0.22.1/vendor/libuv/src/unix/core.c", "uvloop-0.22.1/vendor/libuv",
         ["include", "src", "src/unix"], ["-D_GNU_SOURCE"])]
FLAGS = ["-std=gnu11"]
QUERIES = ["", "a", "lea", "leaf", "lf", "call", "CALL", "str", "luaL", "uv_", "init", "Point", "x"]
# more, for one file: the letter-digit segment rule (B4_DESIGN 7, 18)
EXTRA = {"wsdigits.c": "2 6 16 256 lua2 li2 i2s int2 2str 2s utf8 u8 ud u8d dec sha s256 sh2 256s sum a2 2b a2b ab b16 bu16 b1 buf1 x509 x5 xs x509s store ipv4 ip4 i4 ipv v4 v4a gai2 ga2 get2 getaddr m4 m44 mat4 m4x vec3 v3 v3f h264 h2 264 hd ab1 ab12 a1 1b b3 a3 a1b ab3c aB A1 A1B a1b2 abcd ad cd md5 m5 md u x86 x8 86 64 x64 b64 b6 e ip i2i ip46 9 l9 tcp4 t4 t46 s6 tcp46 SHA1 S1 SIZE 1 BIT B64 U8 UM4 max4 M4 lf lc gf gc gaucf hdr hi4 hu8 hs2 hga2 hx8 hab hl LI LIF".split()}


class Srv:
    def __init__(self, cmd, root, env=None):
        self.p = subprocess.Popen(cmd, cwd=root, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, env=env)
        self.q, self.n, self.pending = queue.Queue(), 0, []
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        f = self.p.stdout
        while True:
            hdr, line = {}, f.readline()
            if not line:
                self.q.put(None)
                return
            while line.strip():
                k, _, v = line.decode().partition(":")
                hdr[k.strip().lower()] = v.strip()
                line = f.readline()
            self.q.put(json.loads(f.read(int(hdr["content-length"]))))

    def send(self, m):
        d = json.dumps(dict(m, jsonrpc="2.0")).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(d) + d)
        self.p.stdin.flush()

    def get(self, pred, timeout=180):
        for i, m in enumerate(self.pending):
            if pred(m):
                return self.pending.pop(i)
        end = time.time() + timeout
        while True:
            m = self.q.get(timeout=max(0.1, end - time.time()))
            if m is None:
                raise RuntimeError("server exited")
            if "id" in m and "method" in m:        # a request of the server
                self.send({"id": m["id"], "result": None})
            elif pred(m):
                return m
            else:
                self.pending.append(m)

    def req(self, method, params):
        self.n += 1
        self.send({"id": self.n, "method": method, "params": params})
        i = self.n
        r = self.get(lambda m: m.get("id") == i and "method" not in m)
        return r.get("result")

    def note(self, method, params):
        self.send({"method": method, "params": params})

    def close(self):
        try:
            self.req("shutdown", None)
            self.note("exit", None)
            self.p.stdin.close()
            self.p.wait(timeout=30)
        except Exception:
            self.p.kill()


def start(kind, cer, root, path, text):
    cmd = [cer, "lsp"] if kind == "cereal" else ["clangd", "--log=error", "--background-index=false"]
    s = Srv(cmd, root)
    uri = "file://" + path
    caps = {"textDocument": {"inlayHint": {}, "callHierarchy": {}, "documentSymbol":
                             {"hierarchicalDocumentSymbolSupport": False}}}
    s.req("initialize", {"rootUri": "file://" + root, "capabilities": caps})
    s.note("initialized", {})
    s.note("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "c",
                                                      "version": 1, "text": text}})
    if kind == "cereal":
        s.req("cereal/waitIdle", {})
    return s, uri


def lines_of(r):
    return (r["start"]["line"], r["start"]["character"])


def hint_label(h):
    l = h["label"]
    return l if isinstance(l, str) else "".join(x["value"] for x in l)


def top_symbols(s, uri, text):
    """(name, position of the name) of the functions and variables clangd lists."""
    out, lines = [], text.split("\n")
    for x in s.req("textDocument/documentSymbol", {"textDocument": {"uri": uri}}) or []:
        if x.get("kind") in (12, 13):
            ln, ch = lines_of(x["location"]["range"])
            m = re.compile(r"\b%s\b" % re.escape(x["name"])).search(lines[ln], ch)
            if m:
                out.append((x["name"], (ln, m.start())))
    return out


def wait_indexed(s, uri, name):
    # clangd: the file's diagnostics come after its preamble (the headers' symbols) is indexed
    s.get(lambda m: m.get("method") == "textDocument/publishDiagnostics" and m["params"]["uri"] == uri)
    end = time.time() + 120               # and the dynamic index has the file
    while time.time() < end:
        if any(x["name"] == name for x in s.req("workspace/symbol", {"query": name}) or []):
            return
        time.sleep(0.05)


def norm_calls(items, rk):
    """name -> sorted site positions (the positions of one caller or callee)."""
    d = {}
    for c in items or []:
        o = c.get("from") or c.get("to")
        d.setdefault(o["name"], set()).update(lines_of(r) for r in c["fromRanges"])
    return d


def collect(kind, cer, root, path, text, syms):
    s, uri = start(kind, cer, root, path, text)
    try:
        res = {"hints": None, "calls": {}}
        if kind == "clangd" and syms:
            wait_indexed(s, uri, syms[0][0])
        end = text.count("\n") + 1
        h = s.req("textDocument/inlayHint", {"textDocument": {"uri": uri}, "range": {
            "start": {"line": 0, "character": 0}, "end": {"line": end, "character": 0}}})
        res["hints"] = {((x["position"]["line"], x["position"]["character"]), hint_label(x)) for x in h or [] if x.get("kind") == 2}
        res["syms"] = {}
        res["queries"] = QUERIES + EXTRA.get(os.path.basename(path), [])
        for q in res["queries"]:
            r = s.req("workspace/symbol", {"query": q}) or []
            for _ in range(200):              # clangd: until its answer stops changing
                r2 = s.req("workspace/symbol", {"query": q}) or []
                if r2 == r:
                    break
                r = r2
            res["syms"][q] = {(x["name"], x["kind"], x["location"]["uri"][7:], lines_of(x["location"]["range"]))
                              for x in r}
            res["syms"][q, "n"] = len(r)
        for name, pos in syms:
            its = s.req("textDocument/prepareCallHierarchy",
                        {"textDocument": {"uri": uri}, "position": {"line": pos[0], "character": pos[1]}})
            r = {"prep": bool(its)}
            for it in (its or [])[:1]:
                r["in"] = norm_calls(s.req("callHierarchy/incomingCalls", {"item": it}), "in")
                r["out"] = norm_calls(s.req("callHierarchy/outgoingCalls", {"item": it}), "out")
            res["calls"][(name, pos)] = r
        return res, top_symbols(s, uri, text) if kind == "clangd" else None
    finally:
        s.close()


def at(lines, pos, n=1):
    return lines[pos[0]][pos[1]:pos[1] + n] if pos[0] < len(lines) else ""


def enclosing_call(lines, pos):
    """The identifier before the unmatched '(' left of pos (same statement), or ''."""
    txt = "\n".join(lines[max(0, pos[0] - 8):pos[0]] + [lines[pos[0]][:pos[1]]])
    depth = 0
    for i in range(len(txt) - 1, -1, -1):
        c = txt[i]
        if c in ")]}":
            depth += 1
        elif c in "([{":
            if depth == 0:
                m = re.search(r"([A-Za-z_]\w*)\s*\)?\s*$", txt[:i])
                return m.group(1) if m else ""
            depth -= 1
        elif c == ";":
            break
    return ""


def classify_hint(side, h, lines, text, macros, sysnames):
    pos, label = h
    callee = enclosing_call(lines, pos)
    arg = re.match(r"\w+", lines[pos[0]][pos[1]:]) if pos[0] < len(lines) else None
    if side == "clangd" and (callee in macros or callee.isupper()):   # CPU_COUNT: a system header's
        return "H1"
    if side == "cereal" and arg and arg.group(0) in macros:
        return "H2"
    if callee and re.search(r"\(\s*\*\s*%s\s*\)\s*\(" % re.escape(callee), text):
        return "H3"
    if re.search(r"\(\s*[*&]\s*\w+\s*\)\s*\([^)]*$", lines[pos[0]][:pos[1] + 1]):
        return "H6"
    if callee and callee in sysnames:
        return "H5"
    return None


def classify_call(side, kind, caller, name, sites, other_sites, lines, text, macros):
    """Why a (name -> sites) entry of one server differs from the other's."""
    mac = re.compile(r"\b(%s)\b" % "|".join(map(re.escape, sorted(macros)))) if macros else None
    if side == "clangd":
        for p in sites:        # a reference that is not a call (fp = leaf, &f)
            m = re.match(r"\w+\s*(.?)", lines[p[0]][p[1]:])
            if not m or m.group(1) != "(":
                return "C1"
    if side == "cereal" and name.startswith("_"):
        return "C6"            # callee spelled in a system header's macro (assert, errno)
    if side == "cereal" and not re.search(r"^[A-Za-z_][\w \t\*]*\b%s\s*\([^;{]*\)\s*[;{]" % re.escape(name),
                                          text, re.M):
        return "C3"
    if other_sites and mac and any(mac.search(lines[p[0]]) for p in sites):
        return "C5"
    return None


def is_system(path):
    return path.startswith("/usr/") or "/sys/" in path or "/include/sys/" in path


def compare_syms(rel, ours, theirs):
    """workspace/symbol: the symbols of the main file must agree exactly.  W1 system symbols
    (clangd only); W2 symbols of a user header only cereal lists (clangd's index has them partly:
    forward declarations, include guards, nothing at all when its preamble does not build); W3 kinds
    (macro 15, struct 5, enumerator 10 for clangd); W4 where a server's answer was cut at the limit
    (the sets then differ by ranking)."""
    out = []
    for q in ours["queries"]:
        o, t = ours["syms"][q], theirs["syms"][q]
        cut = ours["syms"][q, "n"] >= 100 or theirs["syms"][q, "n"] >= 100
        tpos = {(n, p, f): k for n, k, f, p in t}
        opos = {(n, p, f): k for n, k, f, p in o}
        for side, a, b, bpos in (("cereal", o, t, tpos), ("clangd", t, o, opos)):
            for n, k, f, p in sorted(a):
                key = (n, p, f)
                where = "%s:%d:%d" % (os.path.basename(f), p[0] + 1, p[1] + 1)
                if key in bpos:
                    if side == "cereal" and bpos[key] != k:
                        cls = "W3" if (bpos[key], k) in ((15, 12), (15, 14), (5, 23), (10, 22)) else None
                        out.append("%s query %r: %s %s kind %d vs clangd %d [%s]" % (rel, q, n, where, k, bpos[key], cls or "UNEXPECTED"))
                    continue
                cls = ("W1" if side == "clangd" and is_system(f) else
                       "W2" if side == "cereal" and os.path.basename(f) != rel and not is_system(f) else
                       "W4" if cut else None)
                out.append("%s query %r: %s %s (kind %d) only in %s [%s]" % (rel, q, n, where, k, side, cls or "UNEXPECTED"))
    return out


def hdrs(d):
    """The text of the headers next to the file (their #defines are macros too)."""
    return "\n".join(open(os.path.join(d, f)).read() for f in sorted(os.listdir(d)) if f.endswith(".h"))


def compare(rel, ours, theirs, text, headers=""):
    lines = text.split("\n")
    macros = set(re.findall(r"^\s*#\s*define\s+(\w+)", text + headers, re.M))
    sysnames = set(re.findall(r"\b(mem\w+|str\w+|printf|puts|abs|malloc|free|qsort|getenv)\s*\(", text))
    out = []

    def emit(pos, what, cls):
        out.append("%s:%d:%d %s [%s]" % (rel, pos[0] + 1, pos[1] + 1, what, cls or "UNEXPECTED"))
    for side, a, b in (("cereal", ours["hints"], theirs["hints"]), ("clangd", theirs["hints"], ours["hints"])):
        for h in sorted(a - b):
            if side == "cereal" and any(p == h[0] for p, _ in b):
                emit(h[0], "hint %s vs clangd %s" % (h[1], [l for p, l in b if p == h[0]][0]), "H4")
            elif side == "clangd" and any(p == h[0] for p, _ in b):
                continue
            else:
                emit(h[0], "hint %s only in %s" % (h[1], side), classify_hint(side, h, lines, text, macros, sysnames))
    for key in sorted(set(ours["calls"]) | set(theirs["calls"])):
        o, t = ours["calls"].get(key, {}), theirs["calls"].get(key, {})
        if o.get("prep") != t.get("prep"):
            emit(key[1], "prepare on %s: cereal %s, clangd %s" % (key[0], o.get("prep"), t.get("prep")), None)
            continue
        for k in ("in", "out"):
            a, b = o.get(k, {}), t.get(k, {})
            for side, x, y in (("cereal", a, b), ("clangd", b, a)):
                for name in sorted(x):
                    if x[name] == y.get(name):
                        continue
                    other = y.get(name)
                    cls = classify_call(side, k, key[0], name, x[name] - (other or set()), other, lines, text, macros)
                    if other and x[name] < other:
                        continue            # reported from the side with more sites
                    emit(key[1], "%s of %s: %s %s only at %s%s" % (
                        "incoming" if k == "in" else "outgoing", key[0], name, side,
                        sorted((p[0] + 1, p[1] + 1) for p in x[name] - (other or set())),
                        "" if not other else " (also in the other)"), cls)
    return out + compare_syms(rel, ours, theirs)


# Project mode (docs/B5_DESIGN.md 14.6): references, definition and declaration over a
# whole corpus project, each server with a compile_commands.json and its project index
# complete (cereal: cereal/indexWait; clangd: its background index's progress ends and
# the answer stops changing).  (root, flags, unit globs, files to ask about)
UV_UNIX = "async core dl fs getaddrinfo getnameinfo linux loop-watcher loop pipe poll posix-hrtime " \
          "posix-poll process procfs-exepath proctitle random-devurandom random-getrandom signal " \
          "stream sysinfo-loadavg sysinfo-memory tcp thread tty udp".split()
# clangd's background index of a project is not deterministic (whole units' references are
# sometimes missing from a run, and stay missing for it), so its answers are the union of
# CLANGD_RUNS runs, each indexing a fresh copy.
CLANGD_RUNS = 3
PROJECTS = [("lupa-2.8/third-party/lua54", ["-std=gnu11", "-I."], ["*.c"],
             ["lapi.c", "lstring.c", "ltable.c", "lauxlib.c"]),
            ("uvloop-0.22.1/vendor/libuv", ["-std=gnu11", "-D_GNU_SOURCE", "-Iinclude", "-Isrc", "-Isrc/unix"],
             ["src/*.c"] + ["src/unix/%s.c" % n for n in UV_UNIX],
             ["src/uv-common.c", "src/unix/stream.c", "src/unix/loop.c"])]
CALLISH = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
NOT_CALLS = {"if", "while", "for", "switch", "return", "sizeof", "defined", "int", "char", "void",
             "static", "unsigned", "long", "const", "struct"}


def proj_units(root, globs):
    import glob
    return sorted(f for g in globs for f in glob.glob(os.path.join(root, g)))


def proj_server(kind, cer, root):
    if kind == "cereal":
        env = dict(os.environ, CEREAL_INDEX_ENABLE="1", CEREAL_INDEX_CACHEDIR=os.path.join(root, ".cache-cereal"))
        s = Srv([cer, "lsp"], root, env)
    else:
        s = Srv(["clangd", "--log=error", "--background-index", "-j=1"], root)
    s.req("initialize", {"rootUri": "file://" + root, "capabilities": {
        "window": {"workDoneProgress": True}, "textDocument": {"documentSymbol": {"hierarchicalDocumentSymbolSupport": False}}}})
    s.note("initialized", {})
    return s


def proj_wait(kind, s, root, units, uri, text):
    """Both servers' project indexes complete."""
    if kind == "cereal":
        s.req("cereal/indexWait", {})
        return
    s.note("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "c", "version": 1, "text": text}})
    s.get(lambda m: m.get("method") == "textDocument/publishDiagnostics" and m["params"]["uri"] == uri)
    # clangd's progress ends in batches: wait until every unit has a shard in its index directory
    idx = os.path.join(root, ".cache", "clangd", "index")
    want = {os.path.basename(u) for u in units}
    end, begun = time.time() + 900, set()
    while time.time() < end:
        while True:                                # the progress messages seen so far
            try:
                m = s.get(lambda m: m.get("method") == "$/progress", timeout=0.1)
            except Exception:
                break
            v = m["params"]["value"]
            if v.get("kind") == "begin":
                begun.add(m["params"]["token"])
            elif v.get("kind") == "end":
                begun.discard(m["params"]["token"])
        have = {f.rsplit(".", 2)[0] for f in (os.listdir(idx) if os.path.isdir(idx) else [])}
        if want <= have and not begun:             # every shard written, the queue idle (the merged index rebuilt)
            break
    else:
        raise RuntimeError("clangd's background index did not finish")
    s.note("textDocument/didClose", {"textDocument": {"uri": uri}})


def proj_collect(kind, cer, root, units, queries, syms):
    """One server over the project; the answers for the open files in `queries`.  syms: the
    (name, pos) list per file (from clangd's documentSymbol), None to take them from this server."""
    s = proj_server(kind, cer, root)
    mysyms = {}
    try:
        first = os.path.join(root, queries[0])
        proj_wait(kind, s, root, units, "file://" + first, open(first).read())
        prev, agree = None, 0
        for _ in range(40):          # clangd: sweeps repeat until three agree (its merged index lags its shards)
            res = proj_sweep(kind, s, root, queries, syms, mysyms)
            agree = agree + 1 if res == prev else 0
            if kind == "cereal" or agree >= 2:
                break
            prev, syms = res, syms or mysyms
            time.sleep(1)
        return res, mysyms
    finally:
        s.close()


def proj_sweep(kind, s, root, queries, syms, mysyms):
    res = {}
    for rel in queries:
        path = os.path.join(root, rel)
        text, uri = open(path).read(), "file://" + path
        lines = text.split("\n")
        s.note("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "c", "version": 1, "text": text}})
        if kind == "cereal":
            s.req("cereal/waitIdle", {})
        else:
            s.get(lambda m: m.get("method") == "textDocument/publishDiagnostics" and m["params"]["uri"] == uri)
        if syms is None:
            mysyms[rel] = top_symbols(s, uri, text)
        todo = [("references", n, p) for n, p in (syms or mysyms)[rel]]
        seen = set()
        for i, ln in enumerate(lines):
            for m in CALLISH.finditer(ln):
                if m.group(1) not in NOT_CALLS and m.group(1) not in seen and len(seen) < 80 and \
                        not ln.lstrip().startswith(("#", "/*", "*")):
                    seen.add(m.group(1))
                    todo += [("definition", m.group(1), (i, m.start(1))), ("declaration", m.group(1), (i, m.start(1)))]
        for what, name, pos in todo:
            par = {"textDocument": {"uri": uri}, "position": {"line": pos[0], "character": pos[1]}}
            if what == "references":
                par["context"] = {"includeDeclaration": True}
            r = s.req("textDocument/" + what, par) or []
            for _ in range(600 if kind == "clangd" else 0):   # until clangd's answer stops growing
                r2 = s.req("textDocument/" + what, par) or []
                if r2 == r:
                    break
                r = r2
                time.sleep(0.05)
            if isinstance(r, dict):
                r = [r]
            res[rel, what, name, pos] = {(x.get("uri") or x["targetUri"])[7:].replace(root, ".") + ":%d:%d" % lines_of(
                x.get("range") or x["targetSelectionRange"]) for x in r}
        s.note("textDocument/didClose", {"textDocument": {"uri": uri}})
    return res


def active_lines(root, flags, rel):
    """0-based lines of the file that survive the preprocessor with the unit's flags (the
    non-blank lines of `cc -E` after its line markers for the file)."""
    txt = subprocess.run(["cc", "-E"] + flags + [rel], cwd=root, capture_output=True, text=True,
                         stdin=subprocess.DEVNULL).stdout
    res, cur, n = set(), False, 0
    for ln in txt.split("\n"):
        m = re.match(r'# (\d+) "([^"]*)"', ln)
        if m:
            cur, n = os.path.normpath(m.group(2)) == os.path.normpath(rel), int(m.group(1))
        else:
            if cur and ln.strip():
                res.add(n - 1)
            n += 1
    return res


def project_mode(cer, cdir, only):
    """The cross-file differential.  Returns (lines, clangd results, unexplained misses)."""
    out, total, miss = [], 0, 0
    for rel, flags, globs, queries in PROJECTS:
        if only and only not in rel:
            continue
        src = os.path.join(cdir, rel)
        if not os.path.isdir(src):
            continue
        d = tempfile.mkdtemp(prefix="clangd_proj")
        try:
            roots = []
            for i in range(CLANGD_RUNS):     # clangd indexes a fresh copy each time; see CLANGD_RUNS
                roots.append(os.path.join(d, "p%d" % i))
                shutil.copytree(src, roots[i], symlinks=True)
                units = proj_units(roots[i], globs)
                json.dump([{"directory": roots[i], "file": u, "arguments": ["cc"] + flags + ["-c", u]} for u in units],
                          open(os.path.join(roots[i], "compile_commands.json"), "w"))
            root, theirs, syms = roots[0], {}, None
            for r in roots:
                got, mine = proj_collect("clangd", cer, r, units, queries, syms)
                syms = syms or mine
                for k, v in got.items():
                    theirs.setdefault(k, set()).update(v)
            ours, _s = proj_collect("cereal", cer, root, units, queries, syms)
            texts, macros = {}, set()
            for dp, _d, fs in os.walk(root):
                for fn in fs:
                    if fn.endswith((".c", ".h")):
                        macros |= set(re.findall(r"^\s*#\s*define\s+(\w+)", open(os.path.join(dp, fn), errors="replace").read(), re.M))

            def line_at(loc):
                p, ln, ch = loc.rsplit(":", 2)
                p = os.path.join(root, p) if p.startswith("./") else p
                if p not in texts:
                    try:
                        texts[p] = open(p).read().split("\n")
                    except OSError:
                        texts[p] = []
                t = texts[p]
                return (t[int(ln)], int(ch)) if int(ln) < len(t) else ("", 0)

            def where(x):
                return x.rsplit(":", 2)[0] + ":%d:%d" % tuple(int(v) + 1 for v in x.rsplit(":", 2)[1:])
            active = {q: active_lines(root, flags, q) for q in queries}
            for key in sorted(set(theirs) | set(ours)):
                f, what, name, pos = key
                a, b = {x for x in ours.get(key, set()) if not is_system(x)}, {x for x in theirs.get(key, set()) if not is_system(x)}
                head = "%s %s %s@%d:%d:" % (f, what, name, pos[0] + 1, pos[1] + 1)
                if pos[0] not in active[f]:
                    # P5: the position is in an inactive preprocessor region: cereal analyses the
                    # unit as compiled and has nothing there; clangd answers lexically.  Not counted.
                    if a != b:
                        out.append("%s clangd %d, cereal %d results in an inactive region [P5]" % (head, len(b), len(a)))
                    continue
                total += len(b)
                if what != "references":
                    # an answer is a miss only when cereal has none; a different place is P3
                    # (clangd answers the declaration at a call and at the definition itself)
                    if b and not a:
                        miss += 1
                        out.append("%s clangd %s, cereal none [UNEXPECTED]" % (head, sorted(map(where, b))))
                    elif a != b:
                        out.append("%s clangd %s, cereal %s [P3]" % (head, sorted(map(where, b)), sorted(map(where, a))))
                    continue
                for x in sorted(b - a):
                    ln, ch = line_at(x)
                    m = re.match(r"\w+", ln[ch:])
                    cls = "P2" if m and m.group(0) in macros and m.group(0) != name else None
                    miss += cls is None
                    out.append("%s clangd only %s [%s]" % (head, where(x), cls or "UNEXPECTED"))
                for x in sorted(a - b):
                    ln, ch = line_at(x)
                    m = re.match(r"\w+", ln[ch:])
                    cls = "P4" if any(re.search(r"\b%s\b" % re.escape(w), ln[:ch]) for w in macros) else None
                    if cls is None and m and m.group(0) in macros and m.group(0) != name:
                        cls = "P6"   # a use through a macro, presented at the outermost invocation
                    out.append("%s cereal only %s [%s]" % (head, where(x), cls or "UNEXPECTED"))
        except Exception as e:
            out.append("%s: FAILED %r [UNEXPECTED]" % (rel, e))
        finally:
            shutil.rmtree(d, ignore_errors=True)
    return out, total, miss


def main():
    a = sys.argv[1:]
    pos = [x for x in a if not x.startswith("--")]
    cer = os.path.abspath(pos[0] if pos else os.path.join(S, "..", "..", "cereal"))
    if not shutil.which("clangd"):
        print("clangd not found")
        return 1
    only_proj = any(x.startswith("--project=") for x in a)       # --project=NAME: that project only
    jobs = [(os.path.join(CORP, f), ["-I" + CORP, "-isystem", os.path.join(CORP, "sys")])
            for f in sorted(os.listdir(CORP)) if f.endswith(".c") and not only_proj]
    if "--no-real" not in a and not only_proj:
        cdir = os.environ.get("CEREAL_CORPUS", os.path.expanduser("~/corpus"))
        for f, root, incs, other in REAL:
            if os.path.exists(os.path.join(cdir, f)):
                jobs.append((os.path.join(cdir, f), [x for i in incs for x in ("-I" + os.path.join(cdir, root, i),)]
                             + ["-I" + os.path.dirname(os.path.join(cdir, f))] + other))
    bad = total = 0
    for src, incl in jobs:
        text = open(src).read()
        d = tempfile.mkdtemp(prefix="clangd_diff")
        try:
            path = os.path.join(d, os.path.basename(src))
            open(path, "w").write(text)
            open(os.path.join(d, "compile_flags.txt"), "w").write("\n".join(FLAGS + incl) + "\n")
            open(os.path.join(d, ".cereal"), "w").write(" ".join(FLAGS + incl) + "\n")
            theirs, syms = collect("clangd", cer, d, path, text, [])
            theirs, syms = collect("clangd", cer, d, path, text, syms)
            ours, _ = collect("cereal", cer, d, path, text, syms)
            diffs = compare(os.path.basename(src), ours, theirs, text, hdrs(os.path.dirname(os.path.abspath(src))))
        except Exception as e:
            diffs = ["%s: FAILED %r [UNEXPECTED]" % (os.path.basename(src), e)]
        finally:
            shutil.rmtree(d, ignore_errors=True)
        total += len(diffs)
        bad += sum(1 for l in diffs if l.endswith("[UNEXPECTED]"))
        print("\n".join(diffs))
    if "--no-project" not in a:
        cdir = os.environ.get("CEREAL_CORPUS", os.path.expanduser("~/corpus"))
        diffs, results, miss = project_mode(cer, cdir, next((x[10:] for x in a if x.startswith("--project=")), ""))
        total += len(diffs)
        bad += sum(1 for l in diffs if l.endswith("[UNEXPECTED]"))
        print("\n".join(diffs))
        print("clangd_diff project mode: %d clangd results, %d unexplained misses (%.2f%%; stop above 2%%)"
              % (results, miss, 100.0 * miss / max(1, results)))
    print("clangd_diff: %d files, %d differences, %d unexpected" % (len(jobs), total, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
