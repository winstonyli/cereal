#!/usr/bin/env python3
"""Drive `cereal lsp` through a scripted session and print a transcript.

usage: lsp_session.py BIN SCENARIO.json [--update] [--flags FLAGS]

--flags adds FLAGS to the .cereal file in the workspace root (from the
initialize request) for the session: e.g. run a scenario with the
parallel path and cells forced, against the same transcript.

The scenario is a JSON list of steps, run in the scenario's directory
(which holds the workspace files); "$ROOT" in any string is replaced by
that directory's absolute path, and in the transcript it is put back.
  {"send": MESSAGE}                    a request (has "id": the response is
                                       awaited and recorded) or notification
  {"open": PATH}                       didOpen of a workspace file
  {"idle": true}                       barrier: wait until the server has no
                                       build queued or running
  {"wait": METHOD, "uri": URI}         record the latest notification METHOD
                                       received for URI (after an idle step:
                                       the current one), else the next
  {"write": PATH, "text": TEXT}        create a workspace file (removed at
                                       the end), e.g. compile_commands.json
  {"note": TEXT}                       a heading in the transcript
Opaque "data" members (call hierarchy items) are left out; lists (semantic
tokens) are kept.
Responses are printed with sorted keys; completion lists are cut to the
labels matching the step's "labels" regex (default: no leading '_').
Without --update the transcript is compared with SCENARIO.expected."""
import json, os, re, subprocess, sys, threading, queue

BIN, SCEN = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
UPDATE = "--update" in sys.argv
FLAGS = sys.argv[sys.argv.index("--flags") + 1] if "--flags" in sys.argv else None
ROOT = os.path.dirname(SCEN)


def subst(v, a, b):
    if isinstance(v, str):
        return v.replace(a, b)
    if isinstance(v, list):
        return [subst(x, a, b) for x in v]
    if isinstance(v, dict):
        return {k: subst(x, a, b) for k, x in v.items()}
    return v


def main():
    steps = subst(json.load(open(SCEN)), "$ROOT", ROOT)
    err = os.environ.get("LSP_STDERR")  # e.g. to collect sanitizer reports
    p = subprocess.Popen([BIN, "lsp"], cwd=ROOT, stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE,
                         stderr=open(err, "a") if err else subprocess.DEVNULL)
    inbox = queue.Queue()

    def reader():
        f = p.stdout
        while True:
            hdr = {}
            line = f.readline()
            if not line:
                inbox.put(None)
                return
            while line.strip():
                k, _, v = line.decode().partition(":")
                hdr[k.strip().lower()] = v.strip()
                line = f.readline()
            body = f.read(int(hdr["content-length"]))
            inbox.put(json.loads(body))

    threading.Thread(target=reader, daemon=True).start()
    pending = []  # notifications not yet consumed

    def send(msg):
        data = json.dumps(msg).encode()
        p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(data) + data)
        p.stdin.flush()

    def next_msg(pred, latest=False):
        hits = [i for i, m in enumerate(pending) if pred(m)]
        if hits:
            m = pending[hits[-1] if latest else hits[0]]
            for i in reversed(hits if latest else hits[:1]):
                pending.pop(i)
            return m
        while True:
            m = inbox.get(timeout=60)
            if m is None:
                raise SystemExit("server exited")
            if pred(m):
                return m
            pending.append(m)

    out = []
    written = []
    restore = []  # (path, text) of files --flags appended to

    def strip_data(v):
        if isinstance(v, list):
            return [strip_data(x) for x in v]
        if isinstance(v, dict):
            return {k: strip_data(x) for k, x in v.items()
                    if k != "data" or isinstance(x, list)}
        return v

    for st in steps:
        if "write" in st:
            path = os.path.join(ROOT, st["write"])
            with open(path, "w") as f:
                f.write(st["text"])
            written.append(path)
            continue
        if "note" in st:
            out.append("## " + st["note"])
        elif "send" in st:
            msg = st["send"]
            msg.setdefault("jsonrpc", "2.0")
            if FLAGS is not None and msg.get("method") == "initialize":
                path = msg["params"]["rootUri"][len("file://"):] + "/.cereal"
                old = open(path).read() if os.path.exists(path) else None
                with open(path, "w") as f:
                    f.write((old or "") + FLAGS + "\n")
                if old is None:
                    written.append(path)
                else:
                    restore.append((path, old))
            send(msg)
            out.append(">> " + msg.get("method", ""))
            if "id" in msg and msg.get("method") != "exit":
                r = next_msg(lambda m: m.get("id") == msg["id"] and "method" not in m)
                res = r.get("result", r.get("error"))
                if isinstance(res, dict) and "items" in res:  # completion
                    pat = re.compile(st.get("labels", r"^[^_]"))
                    res = dict(res, items=[i for i in res["items"]
                                           if pat.search(i["label"])])
                if msg.get("method") == "initialize":
                    res = {"positionEncoding": res["capabilities"]["positionEncoding"],
                           "providers": sorted(k for k in res["capabilities"])}
                res = strip_data(res)
                out.append(json.dumps(res if "result" in r else {"error": res},
                                      sort_keys=True, indent=1))
        elif "idle" in st:
            idle_id = "idle%d" % len(out)
            send({"jsonrpc": "2.0", "id": idle_id, "method": "cereal/waitIdle"})
            next_msg(lambda m: m.get("id") == idle_id)
            out.append(">> (idle)")
        elif "open" in st:
            path = os.path.join(ROOT, st["open"])
            send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                  "params": {"textDocument": {"uri": "file://" + path, "languageId": "c",
                                              "version": 1, "text": open(path).read()}}})
            out.append(">> didOpen " + st["open"])
        elif "wait" in st:
            def uri_of(m):
                pr = m.get("params") or {}
                return pr.get("uri") or (pr.get("textDocument") or {}).get("uri")
            m = next_msg(lambda m: m.get("method") == st["wait"] and uri_of(m) == st["uri"],
                         latest=True)
            out.append("<< %s %s" % (st["wait"], st["uri"]))
            pr = dict(m["params"])
            pr.pop("uri", None)
            pr.pop("textDocument", None)
            out.append(json.dumps(pr, sort_keys=True, indent=1))
    p.stdin.close()
    rc = p.wait(timeout=60)
    for path in written:
        os.remove(path)
    for path, old in restore:
        with open(path, "w") as f:
            f.write(old)
    out.append("exit %d" % rc)
    text = subst("\n".join(out) + "\n", "file://" + ROOT, "file://$ROOT")
    text = text.replace(ROOT, "$ROOT")
    exp = SCEN[:-len(".json")] + ".expected"
    if UPDATE:
        open(exp, "w").write(text)
        return 0
    want = open(exp).read() if os.path.exists(exp) else ""
    if text != want:
        import difflib
        sys.stdout.writelines(difflib.unified_diff(want.splitlines(1), text.splitlines(1),
                                                   "expected", "got"))
        return 1
    return 0


sys.exit(main())
