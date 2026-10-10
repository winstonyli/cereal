#!/usr/bin/env python3
"""Stress `cereal lsp` with edits, each followed at once by requests.

usage: lsp_stress.py BIN FILE [EDITS] [SEED]

Opens FILE (its directory is the workspace), then EDITS times (default 200):
inserts a line at a random line start (sometimes two edits back to back),
and straight away asks for semantic tokens, completion and hover at random
places, so requests meet carried indexes while checks run, are cancelled and
publish.  Checks are not held.  The client declares refreshSupport; refresh
requests are counted.  Not a golden: run it under TSan/ASan and look at the
sanitizer logs (TSAN_OPTIONS/ASAN_OPTIONS log_path).  Exits 0 if every
request was answered and the server exited cleanly."""
import json, os, queue, random, subprocess, sys, threading

BIN, FILE = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
N = int(sys.argv[3]) if len(sys.argv) > 3 else 200
rnd = random.Random(int(sys.argv[4]) if len(sys.argv) > 4 else 1)
ROOT = os.path.dirname(FILE)
URI = "file://" + FILE
text = open(FILE).read()
lines = text.split("\n")

p = subprocess.Popen([BIN, "lsp"], cwd=ROOT, stdin=subprocess.PIPE,
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
        if m.get("method") == "workspace/semanticTokens/refresh":
            refreshes[0] += 1
        elif "method" not in m and m.get("id") in ids:
            ids.discard(m["id"])


td = {"uri": URI}
wait([send("initialize", {"rootUri": "file://" + ROOT, "capabilities": {
    "workspace": {"semanticTokens": {"refreshSupport": True}}}})])
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
    wait([send("textDocument/semanticTokens/full", {"textDocument": td}),
          send("textDocument/completion", {"textDocument": td, "position": pos}),
          send("textDocument/hover", {"textDocument": td, "position": pos})])
wait([send("cereal/waitIdle", {})])
wait([send("shutdown", None)])
send("exit", None, False)
p.stdin.close()
rc = p.wait(timeout=60)
print("%d edits, %d refresh requests, server exit %d" % (version - 1, refreshes[0], rc))
sys.exit(rc)
