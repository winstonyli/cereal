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
H6, W1-W4; found by this script: C6, G1, P1, see section 18), or UNEXPECTED.
workspace/symbol (B4c) is asked the QUERIES below of each server, with one
document open; a result is (name, kind, file, position of the name).
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


class Srv:
    def __init__(self, cmd, root):
        self.p = subprocess.Popen(cmd, cwd=root, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL)
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
        for q in QUERIES:
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
    for p in sorted(sites):
        line = lines[p[0]]
        if mac and mac.match(line[p[1]:]) and re.search(r"\b(%s|%s)\b" % (re.escape(name), re.escape(caller)),
                                                       line[p[1]:]) and not other_sites:
            return "G1"        # a function defined by a macro invocation
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
    for q in QUERIES:
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


def main():
    a = sys.argv[1:]
    pos = [x for x in a if not x.startswith("--")]
    cer = os.path.abspath(pos[0] if pos else os.path.join(S, "..", "..", "cereal"))
    if not shutil.which("clangd"):
        print("clangd not found")
        return 1
    jobs = [(os.path.join(CORP, f), ["-I" + CORP, "-isystem", os.path.join(CORP, "sys")])
            for f in sorted(os.listdir(CORP)) if f.endswith(".c")]
    if "--no-real" not in a:
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
    print("clangd_diff: %d files, %d differences, %d unexpected" % (len(jobs), total, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
