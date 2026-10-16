#!/usr/bin/env python3
"""Stress `cereal lsp` with edits, each followed at once by requests.

usage: lsp_stress.py [--calls] [--hints] [--symbols] [--refs] [--rename] BIN FILE [EDITS] [SEED]

Opens FILE (its directory is the workspace), then EDITS times (default 200):
inserts a line at a random line start (sometimes two edits back to back),
and straight away asks for semantic tokens, completion and hover at random
places, so requests meet carried indexes while checks run, are cancelled and
publish.  Checks are not held.  The client declares refreshSupport; refresh
requests are counted.  Not a golden: run it under TSan/ASan and look at the
sanitizer logs (TSAN_OPTIONS/ASAN_OPTIONS log_path).  Exits 0 if every
request was answered and the server exited cleanly.

--calls  also sends, at a random name followed by `(`, prepareCallHierarchy
         and incomingCalls/outgoingCalls with a hand-built item (positions
         as the client sees the text now, data {name, doc}, no `c`), so
         CP_WAIT and data.doc resolution race the edits (try a file with
         real calls, e.g. tests/lsp/ccalls/main.c or zstd.c).
--hints  also sends textDocument/inlayHint over a random line range.
--symbols also sends workspace/symbol with a random short query (it never
         waits for a check, so it races the publishes).
--refs   also sends references, definition and declaration at a random
         identifier (with indexing on, they read the project's records).
--rename also sends prepareRename and rename at a random name followed by `(`,
         racing the edits and the indexing (refusals and ContentModified
         come back as error responses; nothing is written)."""
import json, os, queue, random, re, subprocess, sys, threading

FLAGS = {a for a in sys.argv[1:] if a.startswith("--")}
ARGS = [a for a in sys.argv[1:] if not a.startswith("--")]
BIN, FILE = os.path.abspath(ARGS[0]), os.path.abspath(ARGS[1])
N = int(ARGS[2]) if len(ARGS) > 2 else 200
rnd = random.Random(int(ARGS[3]) if len(ARGS) > 3 else 1)
CALLISH = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
NOT_CALLS = {"if", "while", "for", "switch", "return", "sizeof", "defined"}
ROOT = os.path.dirname(FILE)
URI = "file://" + FILE
text = open(FILE).read()
lines = text.split("\n")

ENV = dict(os.environ)
ENV.setdefault("CEREAL_INDEX_ENABLE", "0")    # CEREAL_INDEX_ENABLE=1 to stress with indexing
p = subprocess.Popen([BIN, "lsp"], cwd=ROOT, env=ENV, stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE)
inbox = queue.Queue()


def reader():
    f = p.stdout
    while True:
        line = f.readline()
        if not line:
            inbox.put(None)
            return
        hdr = {}
        while line.strip():
            k, _, v = line.decode().partition(":")
            hdr[k.strip().lower()] = v.strip()
            line = f.readline()
        inbox.put(json.loads(f.read(int(hdr["content-length"]))))


threading.Thread(target=reader, daemon=True).start()
nid = [0]
refreshes = [0]
errors = [0]


def send(method, params, req=True):
    msg = {"jsonrpc": "2.0", "method": method, "params": params}
    if req:
        nid[0] += 1
        msg["id"] = nid[0]
    data = json.dumps(msg).encode()
    p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(data) + data)
    p.stdin.flush()
    return nid[0] if req else None


def wait(ids):
    ids = set(ids)
    while ids:
        m = inbox.get(timeout=120)
        if m is None:
            raise SystemExit("server exited")
        if m.get("method") in ("workspace/semanticTokens/refresh",
                               "workspace/inlayHint/refresh"):
            refreshes[0] += 1
        elif "method" not in m and m.get("id") in ids:
            ids.discard(m["id"])
            if "error" in m:
                errors[0] += 1
                print("error response:", m["error"], file=sys.stderr)


td = {"uri": URI}
wait([send("initialize", {"rootUri": "file://" + ROOT, "capabilities": {
    "workspace": {"semanticTokens": {"refreshSupport": True},
                  "inlayHint": {"refreshSupport": True}},
    "textDocument": {"inlayHint": {}}}})])
send("initialized", {}, False)
send("textDocument/didOpen", {"textDocument": {
    "uri": URI, "languageId": "c", "version": 1, "text": text}}, False)
wait([send("cereal/waitIdle", {})])
version = 1
for i in range(N):
    for _ in range(2 if rnd.random() < 0.2 else 1):
        ln = rnd.randrange(len(lines))
        version += 1
        lines.insert(ln, "int stress_v%d;" % i)
        send("textDocument/didChange", {
            "textDocument": {"uri": URI, "version": version},
            "contentChanges": [{
                "range": {"start": {"line": ln, "character": 0},
                          "end": {"line": ln, "character": 0}},
                "text": "int stress_v%d;\n" % i}]}, False)
    ln = rnd.randrange(len(lines))
    pos = {"line": ln, "character": rnd.randrange(len(lines[ln]) + 1)}
    reqs = [send("textDocument/semanticTokens/full", {"textDocument": td}),
            send("textDocument/completion", {"textDocument": td, "position": pos}),
            send("textDocument/hover", {"textDocument": td, "position": pos})]
    if "--calls" in FLAGS:
        for _ in range(8):
            ln = rnd.randrange(len(lines))
            ms = [m for m in CALLISH.finditer(lines[ln])
                  if m.group(1) not in NOT_CALLS]
            if not ms:
                continue
            m = rnd.choice(ms)
            sel = {"start": {"line": ln, "character": m.start(1)},
                   "end": {"line": ln, "character": m.end(1)}}
            item = {"name": m.group(1), "kind": 12, "uri": URI,
                    "range": {"start": {"line": ln, "character": 0},
                              "end": {"line": ln, "character": len(lines[ln])}},
                    "selectionRange": sel,
                    "data": {"name": m.group(1), "doc": URI}}
            reqs += [send("textDocument/prepareCallHierarchy", {
                         "textDocument": td, "position": sel["start"]}),
                     send("callHierarchy/incomingCalls", {"item": item}),
                     send("callHierarchy/outgoingCalls", {"item": item})]
            break
    if "--refs" in FLAGS:
        for m_ in ("textDocument/references", "textDocument/definition",
                   "textDocument/declaration"):
            ln = rnd.randrange(len(lines))
            ms = list(re.finditer(r"[A-Za-z_]\w*", lines[ln]))
            if ms:
                m = rnd.choice(ms)
                params = {"textDocument": td, "position": {"line": ln, "character": m.start()}}
                if m_.endswith("references"):
                    params["context"] = {"includeDeclaration": rnd.random() < 0.5}
                reqs.append(send(m_, params))
    if "--rename" in FLAGS:
        ln = rnd.randrange(len(lines))
        ms = [m for m in CALLISH.finditer(lines[ln]) if m.group(1) not in NOT_CALLS]
        if ms:
            at = {"line": ln, "character": rnd.choice(ms).start(1)}
            reqs += [send("textDocument/prepareRename", {"textDocument": td, "position": at}),
                     send("textDocument/rename", {"textDocument": td, "position": at,
                                                  "newName": "stress_r%d" % i})]
    if "--hints" in FLAGS:
        a = rnd.randrange(len(lines))
        b = min(len(lines) - 1, a + rnd.randrange(60))
        reqs.append(send("textDocument/inlayHint", {"textDocument": td, "range": {
            "start": {"line": a, "character": 0},
            "end": {"line": b, "character": len(lines[b])}}}))
    if "--symbols" in FLAGS:
        reqs.append(send("workspace/symbol", {"query": rnd.choice(
            ["", "s", "st", "stress", "v1", "main", "_", "lea"])}))
    wait(reqs)
wait([send("cereal/waitIdle", {})])
wait([send("shutdown", None)])
send("exit", None, False)
p.stdin.close()
rc = p.wait(timeout=60)
print("%d edits, %d refresh requests, %d error responses, server exit %d"
      % (version - 1, refreshes[0], errors[0], rc))
sys.exit(rc)
