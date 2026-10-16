#!/usr/bin/env python3
"""Stop-rule measurement for cross-TU rename (docs/B5_DESIGN.md 15, B5d).

Renames every external function defined in libuv (the clangd_diff project: the Linux
units) to a fresh name through `cereal lsp` with the project index complete, and counts
the outcomes.  Nothing is written: a rename only returns edits.

  edits      the rename was planned and the edits returned
  by design  refused for a reason the design lists (macro-body use R4/R7, an ambiguous
             place R1, a collision R6, an incomplete or capped project R2/R5, a use in
             an inactive #if group A3.9, ...); the script prints each kind's count, so the
             strict reading (A3.9 counted as false) is the sum of the two lines
  false      any other refusal, or an error: a refusal that should not happen

The stop rule: false refusals > 10% of attempts.  Usage:
  rename_libuv.py [CEREAL] [--verbose]      (CEREAL_CORPUS: the corpus directory)
Exit status 1 when the stop rule fires."""
import json, os, re, shutil, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import clangd_diff as cd

# refusal messages that are the design working (docs/B5_DESIGN.md 11.5): a regexp each
BY_DESIGN = [
    ("macro", r"#define body|macro"),
    # B2's A3.9, inherited by the gate: a use in a group the unit does not compile (another
    # platform's branch) would be left behind by the rename, so refusing is right
    ("inactive", r"inactive #if group"),
    ("ambiguous", r"ambiguous|more than one|also names"),
    ("collision", r"collid|already|conflict|would change|differs|shadow|capture"),
    ("scope", r"index is not complete|not complete|too many|more than \d+|translation units"),
    ("unplaced", r"could not be placed|not be located|changed|stale"),
]
STOP = 0.10


def externals(s, root, rel):
    """(name, position) of the external functions defined in the file: its documentSymbol
    functions whose declaration line is not static."""
    path = os.path.join(root, rel)
    text = open(path).read()
    lines = text.split("\n")
    uri = "file://" + path
    s.note("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "c", "version": 1, "text": text}})
    s.req("cereal/waitIdle", {})
    out = []
    for x in s.req("textDocument/documentSymbol", {"textDocument": {"uri": uri}}) or []:
        if x.get("kind") != 12:
            continue
        r = x["selectionRange"]["start"]
        head = " ".join(lines[max(0, r["line"] - 2):r["line"] + 1])
        if re.search(r"\bstatic\b", head.split(x["name"])[0].split(";")[-1].split("}")[-1]):
            continue
        out.append((x["name"], uri, r["line"], r["character"]))
    return out


def classify(msg):
    for why, rx in BY_DESIGN:
        if re.search(rx, msg, re.I):
            return why
    return "false"


def main():
    a = sys.argv[1:]
    pos = [x for x in a if not x.startswith("--")]
    cer = os.path.abspath(pos[0] if pos else os.path.join(cd.S, "..", "..", "cereal"))
    verbose = "--verbose" in a
    cdir = os.environ.get("CEREAL_CORPUS", os.path.expanduser("~/corpus"))
    rel, flags, globs, _q = [p for p in cd.PROJECTS if "libuv" in p[0]][0]
    src = os.path.join(cdir, rel)
    d = tempfile.mkdtemp(prefix="rename_libuv", dir=os.path.expanduser("~"))
    tally, msgs, edits_n = {}, {}, 0
    try:
        root = os.path.join(d, "p")
        shutil.copytree(src, root, symlinks=True)
        # a unit that does not compile here (posix-poll.c is for other platforms) is PARTIAL by
        # nature and R2 refuses every rename while it is in the project: leave it out, as a
        # user would with the `paths` setting
        units = [u for u in cd.proj_units(root, globs) if subprocess.run(
            ["cc"] + flags + ["-fsyntax-only", u], cwd=root, stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0]
        json.dump([{"directory": root, "file": u, "arguments": ["cc"] + flags + ["-c", u]} for u in units],
                  open(os.path.join(root, "compile_commands.json"), "w"))
        s = cd.proj_server("cereal", cer, root)
        try:
            s.req("cereal/indexWait", {})
            todo = []
            for u in units:
                todo += externals(s, root, os.path.relpath(u, root))
            seen = set()
            for name, uri, line, ch in todo:
                if name in seen:
                    continue
                seen.add(name)
                s.note("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "c", "version": 1,
                                                                  "text": open(uri[7:]).read()}})
                s.n += 1
                i = s.n
                s.send({"id": i, "method": "textDocument/rename", "params": {
                    "textDocument": {"uri": uri}, "position": {"line": line, "character": ch},
                    "newName": "zz_" + name}})
                m = s.get(lambda m: m.get("id") == i and "method" not in m, timeout=300)
                if "error" in m:
                    msg = m["error"]["message"]
                    why = classify(msg)
                    msgs.setdefault(why, []).append("%s: %s" % (name, msg))
                else:
                    why = "edits"
                    edits_n += 1
                tally[why] = tally.get(why, 0) + 1
        finally:
            s.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
    total = sum(tally.values())
    for k in sorted(tally):
        print("%-10s %4d" % (k, tally[k]))
        if verbose or k == "false":
            for x in msgs.get(k, [])[:60]:
                print("    " + x)
    bad = tally.get("false", 0)
    rate = bad / total if total else 1.0
    print("strict reading (inactive-group refusals counted as false): %.1f%%" % (
        100 * (bad + tally.get("inactive", 0)) / max(total, 1)))
    print("attempts %d, false refusals %d (%.1f%%), stop rule at %.0f%%: %s" % (
        total, bad, 100 * rate, 100 * STOP, "FIRED" if rate > STOP else "ok"))
    return 1 if rate > STOP else 0


if __name__ == "__main__":
    sys.exit(main())
