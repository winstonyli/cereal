#!/usr/bin/env python3
"""Native oracle for the x86-64 back end (src/c/cgx86.c).

Usage: asm_exec.py CEREAL FILE.c...

For each FILE.c: `cereal check --dump-asm` writes assembly for the functions
the generator supports (and `# x86: name unsupported: why` for the rest); the
assembly is linked with a driver that calls each function on random integer
arguments, and its output is compared with the same functions compiled by gcc
(-O0 -fwrapv).  The exit status is 1 on any mismatch, a link failure, or fewer
than half of the integer functions emitted.  Covers what cgx86.c supports
(integers, pointers, locals, arrays, calls to the file's own functions);
everything else is reported as unsupported, not skipped silently."""
import os, random, re, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cfg_exec as X


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, stdin=subprocess.DEVNULL, **kw)


def main():
    cereal, files = sys.argv[1], sys.argv[2:]
    random.seed(4321)
    bad = 0
    for path in files:
        asm = run([cereal, "check", "--target=x86_64-linux-gnu", "--dump-asm", path]).stdout
        funcs = X.parse(run([cereal, "check", "--target=x86_64-linux-gnu", "--dump-cfg", path]).stdout)
        notes = dict(re.findall(r"^# x86: (\S+) unsupported: (.*)$", asm, re.M))
        todo = []
        for f in funcs.values():
            try:
                X.ity(f.ret)
                for p in f.params:
                    X.ity(p)
            except X.Skip:
                continue
            todo.append(f)
        done = [f for f in todo if f.name not in notes]
        vecs = {f.name: [[X.values(p) for p in f.params] for _ in range(24)] for f in done}
        calls = "".join('    printf("%%lld\\n", (long long)%s(%s));\n' % (
            f.name, ", ".join(str(x) + ("LL" if abs(x) > 2**31 - 1 else "") for x in v))
            for f in done for v in vecs[f.name])
        main_c = "#include <stdio.h>\nint main(void)\n{\n" + calls + "    return 0;\n}\n"
        protos = "".join("%s %s(%s);\n" % (f.ret, f.name, ", ".join(f.params) or "void") for f in done)
        with tempfile.TemporaryDirectory() as td:
            j = lambda x: os.path.join(td, x)
            open(j("ref.c"), "w").write(open(path).read() + "\n" + main_c)
            open(j("mine.c"), "w").write(protos + main_c)
            open(j("a.s"), "w").write(asm)
            r = run(["gcc", "-O0", "-fwrapv", "-w", "-o", j("ref"), j("ref.c")])
            if r.returncode:
                print(path + ": gcc failed\n" + r.stderr[:400])
                bad += 1
                continue
            r = run(["gcc", "-w", "-o", j("mine"), j("mine.c"), j("a.s")])
            if r.returncode:
                print(path + ": assemble/link failed\n" + r.stderr[:800])
                bad += 1
                continue
            a = run([j("ref")], timeout=60).stdout.split()
            try:
                b = run([j("mine")], timeout=60).stdout.split()
            except subprocess.TimeoutExpired:
                print(path + ": generated code timed out (loop never ends?)")
                bad += 1
                continue
        i, wrong = 0, 0
        for f in done:
            n = len(vecs[f.name])
            for v, x, y in zip(vecs[f.name], a[i:i + n], b[i:i + n]):
                if x != y:
                    print("%s: %s%s: gcc %s, ours %s" % (path, f.name, tuple(v), x, y))
                    wrong += 1
                    break
            i += n
        bad += wrong
        print("%s: %d functions compared, %d unsupported%s" % (
            path, len(done) - wrong, len(notes),
            (": " + ", ".join("%s (%s)" % kv for kv in notes.items())) if notes else ""))
        if len(done) * 2 < len(todo):
            print(path + ": fewer than half the integer functions were emitted")
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
