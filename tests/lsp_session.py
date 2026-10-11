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
                                       the current one), else the next;
                                       URI null: notifications without one
  {"waitall": METHOD, "uri": URI}      record, in order, every notification
                                       METHOD for URI not yet recorded (after
                                       an idle step: e.g. both phases' diag-
                                       nostics of one edit)
  {"write": PATH, "text": TEXT}       create a workspace file (removed at
                                       the end), e.g. compile_commands.json
  {"fixall": PATH, "flags": FLAGS}     the source.fixAll.cereal action for the
                                       document applied to its text, compared
                                       with `cereal fix FLAGS` on a copy
  {"note": TEXT}                       a heading in the transcript
  {"env": {NAME: VALUE}}               the server's environment (applied
                                       before it starts, wherever it stands)
Opaque "data" members (call hierarchy items) are left out; lists (semantic
tokens) are kept.
Responses are printed with sorted keys; completion lists are cut to the
labels matching the step's "labels" regex (default: no leading '_').
Without --update the transcript is compared with SCENARIO.expected."""
import json, os, re, shutil, subprocess, sys, tempfile, threading, queue

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
    env = dict(os.environ)
    for st in steps:
        env.update(st.get("env", {}))
    p = subprocess.Popen([BIN, "lsp"], cwd=ROOT, env=env, stdin=subprocess.PIPE,
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
    texts = {}  # path -> the document's text (open, and full-text changes)
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
        if "env" in st:
            continue
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
            if msg.get("method") == "textDocument/didChange":  # full texts only
                for ch in msg["params"]["contentChanges"]:
                    if "range" not in ch:
                        texts[msg["params"]["textDocument"]["uri"][len("file://"):]] = ch["text"]
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
                    caps = res["capabilities"]
                    res = {"positionEncoding": caps["positionEncoding"],
                           "providers": sorted(k for k in caps)}
                    if "codeActionProvider" in caps:
                        res["codeActionProvider"] = caps["codeActionProvider"]
                res = strip_data(res)
                out.append(json.dumps(res if "result" in r else {"error": res},
                                      sort_keys=True, indent=1))
        elif "fixall" in st:
            # source.fixAll.cereal applied to the document's text must equal
            # what `cereal fix` makes of a copy of that text
            path = os.path.join(ROOT, st["fixall"])
            text = texts[path]
            fid = "fixall%d" % len(out)
            send({"jsonrpc": "2.0", "id": fid, "method": "textDocument/codeAction",
                  "params": {"textDocument": {"uri": "file://" + path},
                             "range": {"start": {"line": 0, "character": 0},
                                       "end": {"line": text.count("\n") + 1, "character": 0}},
                             "context": {"diagnostics": [], "only": ["source.fixAll.cereal"]}}})
            res = next_msg(lambda m: m.get("id") == fid)["result"]
            edits = []
            for act in res:
                ed = act["edit"]
                for ch in ed.get("documentChanges", []):
                    edits += ch["edits"]
                for es in ed.get("changes", {}).values():
                    edits += es
            lines = text.split("\n")
            starts = [0]
            for ln in lines[:-1]:
                starts.append(starts[-1] + len(ln) + 1)

            def off(pos):
                return starts[min(pos["line"], len(starts) - 1)] + pos["character"]
            parts, at = [], 0
            for i, e in sorted(enumerate(edits), key=lambda ie: (off(ie[1]["range"]["start"]), off(ie[1]["range"]["end"]), ie[0])):
                parts += [text[at:off(e["range"]["start"])], e["newText"]]
                at = off(e["range"]["end"])
            lsp_text = "".join(parts) + text[at:]
            tmp = tempfile.mkdtemp()
            with open(os.path.join(tmp, "doc.c"), "w") as f:
                f.write(text)
            subprocess.run([BIN, "fix"] + st.get("flags", "").split() + ["doc.c"], cwd=tmp,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            cli_text = open(os.path.join(tmp, "doc.c")).read()
            shutil.rmtree(tmp)
            out.append(">> fix-all of %s: %d edits, %s cereal fix" %
                       (st["fixall"], len(edits),
                        "equal to" if lsp_text == cli_text else "DIFFERENT FROM"))
            out.append(lsp_text.rstrip("\n"))
        elif "idle" in st:
            idle_id = "idle%d" % len(out)
            send({"jsonrpc": "2.0", "id": idle_id, "method": "cereal/waitIdle"})
            next_msg(lambda m: m.get("id") == idle_id)
            out.append(">> (idle)")
        elif "absent" in st:
            seen = any(m.get("method") == st["absent"] for m in pending)
            out.append("!! received " + st["absent"] if seen else ">> (no " + st["absent"] + ")")
        elif "open" in st:
            path = os.path.join(ROOT, st["open"])
            texts[path] = open(path).read()
            send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                  "params": {"textDocument": {"uri": "file://" + path, "languageId": "c",
                                              "version": 1, "text": texts[path]}}})
            out.append(">> didOpen " + st["open"])
        elif "wait" in st or "waitall" in st:
            def uri_of(m):
                pr = m.get("params") or {}
                return pr.get("uri") or (pr.get("textDocument") or {}).get("uri")
            meth = st.get("wait") or st["waitall"]
            pred = lambda m: m.get("method") == meth and uri_of(m) == st["uri"]
            if "wait" in st:
                got = [next_msg(pred, latest=True)]
            else:
                got = [m for m in pending if pred(m)]
                pending[:] = [m for m in pending if not pred(m)]
            for m in got:
                out.append("<< %s %s" % (meth, st["uri"] or "(no uri)"))
                pr = dict(m.get("params") or {})
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
