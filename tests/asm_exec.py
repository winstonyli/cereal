#!/usr/bin/env python3
"""Native oracle for the x86-64 back end (src/c/cgx86.c).

Usage: asm_exec.py CEREAL FILE.c...

For each FILE.c: `cereal check --dump-asm` writes assembly for the functions
the generator supports (and `# x86: name unsupported: why` for the rest); the
assembly is linked with a driver that calls each function on random integer
arguments, and its output is compared with the same functions compiled by gcc
(-O0 -fwrapv).  The exit status is 1 on any mismatch, a link failure, or fewer
than half of the integer functions emitted.  A gcc/FILE.c next to FILE.c is built
by gcc and linked into both programs: its gcc_* functions are driven like the
others, and FILE.c calls its other functions, so structs cross the ABI both ways.  Covers what cgx86.c supports
(integers, pointers, locals, arrays, calls to the file's own functions);
everything else is reported as unsupported, not skipped silently."""
import os, random, re, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cfg_exec as X


FLT = ("float", "double")


def supported(t):
    t = t.strip()
    return t in X.INT or t in FLT


def value(t):
    """A random argument; floats stay small enough that conversions to integers are defined."""
    if t.strip() in FLT:
        return random.choice([0.0, 1.0, -1.0, 0.5, -2.5, 3.25, 7.0, 100.0, -1e3, 1e-3,
                              random.uniform(-500, 500), float(random.randint(-9, 9))])
    return X.values(t)


def literal(v):
    if isinstance(v, float):
        return float(v).hex()
    return str(v) + ("LL" if abs(v) > 2**31 - 1 else "")


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
            if supported(f.ret) and all(supported(p) for p in f.params):
                todo.append(f)
        done = [f for f in todo if f.name not in notes]
        abi = os.path.join(os.path.dirname(path), "gcc", os.path.basename(path))  # gcc-built side
        if os.path.exists(abi):
            cf = X.parse(run([cereal, "check", "--target=x86_64-linux-gnu", "--dump-cfg", abi]).stdout)
            done += [f for f in cf.values() if f.name.startswith("gcc_") and supported(f.ret)
                     and all(supported(p) for p in f.params)]
        ext = [abi] if os.path.exists(abi) else []
        inc = ["-I", os.path.dirname(os.path.abspath(path))]
        vecs = {f.name: [[value(p) for p in f.params] for _ in range(24)] for f in done}
        calls = "".join('    printf("%s\\n", (%s)%s(%s));\n' % (
            "%.17g" if f.ret.strip() in FLT else "%lld",
            "double" if f.ret.strip() in FLT else "long long",
            f.name, ", ".join(literal(x) for x in v))
            for f in done for v in vecs[f.name])
        main_c = "#include <stdio.h>\nint main(void)\n{\n" + calls + "    return 0;\n}\n"
        protos = "".join("%s %s(%s);\n" % (f.ret, f.name, ", ".join(f.params) or "void") for f in done)
        with tempfile.TemporaryDirectory() as td:
            j = lambda x: os.path.join(td, x)
            open(j("ref.c"), "w").write(open(path).read() + "\n" + protos + main_c)
            open(j("mine.c"), "w").write(protos + main_c)
            open(j("a.s"), "w").write(asm)
            r = run(["gcc", "-O0", "-fwrapv", "-w"] + inc + ["-o", j("ref"), j("ref.c")] + ext + ["-lm"])
            if r.returncode:
                print(path + ": gcc failed\n" + r.stderr[:400])
                bad += 1
                continue
            r = run([j("ref")], timeout=60)
            a = r.stdout.split()
            want = sum(len(vecs[f.name]) for f in done)
            if r.returncode or len(a) != want:
                print("%s: the gcc build ran badly (exit %d, %d of %d results)" % (
                    path, r.returncode, len(a), want))
                bad += 1
                continue
            outs = {}
            # the same functions as an object file: bytes against the assembler's, then linked and run
            run([cereal, "check", "--target=x86_64-linux-gnu", "--emit-obj=" + j("o.o"), path])
            r = run(["gcc", "-c", "-w", "-o", j("gas.o"), j("a.s")])
            for o in ("o.o", "gas.o"):
                if os.path.exists(j(o)):
                    run(["objcopy", "-O", "binary", "-j", ".text", j(o), j(o + ".bin")])
            if not os.path.exists(j("o.o")):
                print(path + ": no object written")
                bad += 1
            else:
                ours, gas = open(j("o.o.bin"), "rb").read(), open(j("gas.o.bin"), "rb").read()
                if ours != gas:
                    k = next((n for n in range(min(len(ours), len(gas))) if ours[n] != gas[n]),
                             min(len(ours), len(gas)))
                    print("%s: object .text differs from the assembler's at byte %d (%d vs %d bytes)\n"
                          "  ours %s\n  gas  %s" % (path, k, len(ours), len(gas),
                                                  ours[max(0, k - 8):k + 8].hex(" "),
                                                  gas[max(0, k - 8):k + 8].hex(" ")))
                    bad += 1
            for tag, src in (("asm", "a.s"), ("obj", "o.o")):
                if not os.path.exists(j(src)):
                    continue
                r = run(["gcc", "-w"] + inc + ["-o", j("mine" + tag), j("mine.c"), j(src)] + ext + ["-lm"])
                if r.returncode:
                    print(path + ": assemble/link failed (" + tag + ")\n" + r.stderr[:800])
                    bad += 1
                    continue
                try:
                    r = run([j("mine" + tag)], timeout=60)
                    outs[tag] = r.stdout.split()
                    if r.returncode or len(outs[tag]) != len(a):
                        print("%s: generated code ran badly (%s, exit %d, %d of %d results)" % (
                            path, tag, r.returncode, len(outs[tag]), len(a)))
                        bad += 1
                except subprocess.TimeoutExpired:
                    print(path + ": generated code timed out (loop never ends?)")
                    bad += 1
        wrong = 0
        for tag, b in outs.items():
            i = 0
            for f in done:
                n = len(vecs[f.name])
                for v, x, y in zip(vecs[f.name], a[i:i + n], b[i:i + n]):
                    if x != y:
                        print("%s: %s%s: gcc %s, ours (%s) %s" % (path, f.name, tuple(v), x, tag, y))
                        wrong += 1
                        break
                i += n
        bad += wrong
        print("%s: %d functions compared, %d unsupported%s" % (
            path, len(done), len(notes),
            (": " + ", ".join("%s (%s)" % kv for kv in notes.items())) if notes else ""))
        if len(done) * 2 < len(todo):
            print(path + ": fewer than half the integer functions were emitted")
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
