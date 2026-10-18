#!/usr/bin/env python3
"""Execution oracle for the control-flow graph (docs/TAST_T8_PLAN.md, section 5).

Usage: cfg_exec.py CEREAL FILE.c...

For each FILE.c: read `cereal check --dump-cfg`, interpret every function whose
parameters and result are integers, with locals, arrays, structs and
pointers into them (a cell per object and byte offset; calls only to other
functions of the file), and compare its results on random
arguments with the same functions compiled by gcc (-O0 -fwrapv).  Functions
the interpreter cannot run are listed as skipped; the exit status is 1 on any
mismatch, any function whose graph still holds hidden control flow, or fewer
than half of the functions compared.  What it covers: every statement and
expression lowering of T8a/T8b, and memory access within one object.  What it
does not: floating point, aggregate copies, bit-fields, type punning, pointers
passed in or out, indirect calls (the structural verifier and the corpus gate
only).
"""
import os, random, re, subprocess, sys, tempfile

LIMIT = 200000          # interpreter steps per call


class Skip(Exception):
    pass


mem = {}                # (object, byte offset) -> value; an object is (frame, name)
frames = [0]


INT = {  # type text -> (bits, signed)
    "_Bool": (8, False), "char": (8, True), "signed char": (8, True),
    "unsigned char": (8, False), "short int": (16, True),
    "unsigned short int": (16, False), "short unsigned int": (16, False),
    "long unsigned int": (64, False), "long long unsigned int": (64, False),
    "short signed int": (16, True), "int": (32, True),
    "unsigned int": (32, False), "long int": (64, True),
    "unsigned long int": (64, False), "long long int": (64, True),
    "unsigned long long int": (64, False),
}


def ity(t):
    t = t.strip()
    if t not in INT:
        raise Skip("type " + t)
    return INT[t]


def wrap(v, t):
    if t.strip().endswith("*"):      # a pointer: (object, byte offset) or 0
        return v
    if t.strip() == "_Bool":
        return int(v != 0)
    bits, sg = ity(t)
    v &= (1 << bits) - 1
    return v - (1 << bits) if sg and v >> (bits - 1) else v


NPAY = {"const": 1, "sym": 1, "reg": 1, "unop": 1, "binop": 1, "cmp": 1,
        "local": 1, "call": 1, "ptradd": 1, "ptrdiff": 1, "field": 1}
OPS = {"const", "sym", "reg", "load", "store", "cast", "unop", "binop", "cmp",
       "call", "comma", "post", "exprstmt", "local", "addr", "deref", "ptradd", "ptrdiff",
       "field"}


class Node:
    def __init__(self, line):
        tok = line.split()
        self.id = int(tok[0][1:])
        self.op = tok[1]
        if self.op not in OPS:
            raise Skip("op " + self.op)
        i = 2 + NPAY.get(self.op, 0)
        self.pay = tok[2:i]
        self.kids = []
        while i < len(tok) and tok[i].startswith("#"):
            self.kids.append(int(tok[i][1:]))
            i += 1
        rest = tok[i:]
        if self.op == "call" and (self.pay[0] == "*" or rest[:1] == ["fixed"]):
            raise Skip("indirect or variadic call")
        self.ty = " ".join(rest)


class Block:
    def __init__(self):
        self.nodes = []
        self.term = None


class Func:
    def __init__(self, name, ret, params):
        self.name, self.ret, self.params = name, ret, params
        self.blocks = {}
        self.skip = None
        self.pnames = []


def parse(text):
    funcs, cur, blk = {}, None, None
    for ln in text.split("\n"):
        m = re.match(r"unit \S+ func (\S+) (.*)$", ln)
        if m:
            sig = m.group(2)
            k = sig.index("(")
            ps = sig[k + 1:-1].strip()
            cur = Func(m.group(1).split("=")[0], sig[:k].strip(),
                       [] if ps in ("", "void") else [p.strip() for p in ps.split(",")])
            funcs[cur.name] = cur
            blk = None
            continue
        if ln.startswith("unit "):
            cur = None
            continue
        if cur is None:
            continue
        m = re.match(r"B(\d+):", ln)
        if m:
            blk = Block()
            cur.blocks[int(m.group(1))] = blk
            continue
        if blk is None or not ln.startswith("  "):
            continue
        s = ln.strip()
        try:
            if s.startswith("#"):
                blk.nodes.append(Node(s))
            elif s.startswith("case "):
                _, v, b = s.split()
                blk.term[3].append((int(v), int(b[1:])))
            else:
                t = s.split()
                if t[0] == "jmp":
                    blk.term = ("jmp", None, int(t[1][1:]), None)
                elif t[0] == "br":
                    blk.term = ("br", int(t[1][1:]), int(t[2][1:]), int(t[3][1:]))
                elif t[0] == "swtch":
                    blk.term = ("swtch", int(t[1][1:]), int(t[3][1:]), [])
                elif t[0] == "ret":
                    blk.term = ("ret", int(t[1][1:]) if len(t) > 1 else None, None, None)
                else:
                    raise Skip("terminator " + t[0])
        except Skip as e:
            cur.skip = cur.skip or str(e)
    return funcs


def sym_key(tok):
    if "@" not in tok:
        raise Skip("global " + tok)
    return tok


def run(funcs, f, args, depth=0):
    if f.skip:
        raise Skip(f.skip)
    if depth > 40:
        raise Skip("deep recursion")
    var = mem
    fid = frames[0] = frames[0] + 1
    # parameters: the symbols never declared by a local, in source order
    declared, used = set(), {}
    for b in f.blocks.values():
        for n in b.nodes:
            if n.op == "local":
                declared.add(n.pay[0])
            elif n.op == "sym":
                used[sym_key(n.pay[0])] = 1
    if len(f.pnames) != len(args):
        raise Skip("parameter names")
    for nm, a, t in zip(f.pnames, args, f.params):
        ks = sorted((k for k in used if k not in declared and k.split("@")[0] == nm),
                    key=lambda k: tuple(int(x) for x in k.split("@")[1].split(":")))
        if ks:                          # an unused parameter has no symbol in the graph
            var[((fid, ks[0]), 0)] = wrap(a, t)
    steps, bid, val = 0, min(f.blocks), {}
    while True:
        b = f.blocks[bid]
        val.clear()
        for n in b.nodes:
            steps += 1
            if steps > LIMIT:
                raise Skip("step limit")
            k = [val[x] for x in n.kids]
            o = n.op
            if o == "const":
                val[n.id] = ("v", wrap(int(n.pay[0]), n.ty))
            elif o == "sym":
                val[n.id] = ("r", ((fid, sym_key(n.pay[0])), 0))
            elif o == "reg":
                val[n.id] = ("r", ((fid, "reg" + n.pay[0]), 0))
            elif o == "load":
                key = k[0][1]
                if key not in var:
                    raise Skip("uninitialized %s" % (key,))
                val[n.id] = ("v", var[key])
            elif o == "store":
                val[("old", n.id)] = var.get(k[0][1])
                var[k[0][1]] = wrap(k[1][1], n.ty)
                val[n.id] = ("v", var[k[0][1]])
            elif o == "post":              # the value before the store it wraps
                val[n.id] = ("v", val[("old", n.kids[0])])
            elif o == "cast":
                val[n.id] = ("v", wrap(k[0][1], n.ty))
            elif o == "unop":
                v, p = k[0][1], n.pay[0]
                r = -v if p == "-" else ~v if p == "~" else int(v == 0) if p == "!" else None
                if r is None:
                    raise Skip("unop " + p)
                val[n.id] = ("v", wrap(r, n.ty))
            elif o == "binop":
                a, c, p = k[0][1], k[1][1], n.pay[0]
                if p in "/%" and c == 0:
                    raise Skip("division by zero")
                if p in ("<<", ">>") and not 0 <= c < ity(n.ty)[0]:
                    raise Skip("shift count")
                if p == "/":
                    r = abs(a) // abs(c) * (1 if (a < 0) == (c < 0) else -1)
                elif p == "%":
                    r = a - c * (abs(a) // abs(c) * (1 if (a < 0) == (c < 0) else -1))
                else:
                    r = {"+": lambda: a + c, "-": lambda: a - c, "*": lambda: a * c,
                         "<<": lambda: a << c, ">>": lambda: a >> c, "&": lambda: a & c,
                         "|": lambda: a | c, "^": lambda: a ^ c}[p]()
                val[n.id] = ("v", wrap(r, n.ty))
            elif o == "cmp":
                a, c = k[0][1], k[1][1]
                if isinstance(a, tuple) and isinstance(c, tuple) and a[0] == c[0]:
                    a, c = a[1], c[1]
                elif isinstance(a, tuple) or isinstance(c, tuple):
                    if n.pay[0] not in ("==", "!="):
                        raise Skip("order of unrelated pointers")
                    a, c = 0, 1                 # different objects, or null: never equal
                r = {"<": a < c, ">": a > c, "<=": a <= c, ">=": a >= c,
                     "==": a == c, "!=": a != c}[n.pay[0]]
                val[n.id] = ("v", int(r))
            elif o == "comma":
                val[n.id] = k[1]
            elif o == "exprstmt":
                pass
            elif o == "local":
                if k:
                    var[((fid, sym_key(n.pay[0])), 0)] = wrap(k[0][1], n.ty)
            elif o == "addr":
                val[n.id] = ("v", k[0][1])
            elif o == "deref":
                val[n.id] = ("r", k[0][1])
            elif o == "field":
                val[n.id] = ("r", (k[0][1][0], k[0][1][1] + int(n.pay[0])))
            elif o == "ptradd":
                if not isinstance(k[0][1], tuple):
                    raise Skip("null pointer arithmetic")
                val[n.id] = ("v", (k[0][1][0], k[0][1][1] + int(n.pay[0][1:]) * k[1][1]))
            elif o == "ptrdiff":
                if k[0][1][0] != k[1][1][0]:
                    raise Skip("difference of unrelated pointers")
                val[n.id] = ("v", (k[0][1][1] - k[1][1][1]) // int(n.pay[0][1:]))
            elif o == "call":
                g = funcs.get(n.pay[0])
                if g is None or len(g.params) != len(k):
                    raise Skip("call " + n.pay[0])
                r = run(funcs, g, [x[1] for x in k], depth + 1)
                val[n.id] = ("v", r if r is not None else 0)
        t = b.term
        if t is None:
            raise Skip("no terminator")
        if t[0] == "jmp":
            bid = t[2]
        elif t[0] == "br":
            bid = t[2] if val[t[1]][1] != 0 else t[3]
        elif t[0] == "swtch":
            v = val[t[1]][1]
            bid = dict(t[3]).get(v, t[2])
        else:
            return None if t[1] is None else wrap(val[t[1]][1], f.ret)


def values(t):
    bits, sg = ity(t)
    lo, hi = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if sg else (0, (1 << bits) - 1)
    small = [x for x in range(-6, 7) if lo <= x <= hi]
    return [random.choice(small) if random.random() < 0.7 else random.randint(lo, hi)
            for _ in range(1)][0]


def main():
    cereal, files = sys.argv[1], sys.argv[2:]
    random.seed(1234)
    bad = 0
    for path in files:
        out = subprocess.run([cereal, "check", "--target=x86_64-linux-gnu", "--verify-cfg", path],
                             capture_output=True, text=True, stdin=subprocess.DEVNULL).stdout
        if "verify-cfg:" in out:
            print(path + ": verifier findings")
            bad += 1
        funcs = parse(out)
        src = open(path).read()
        for f in funcs.values():        # parameter names come from the source
            m = re.search(r"\b%s\s*\(([^)]*)\)\s*\{" % f.name, src)
            if m and m.group(1).strip() not in ("", "void"):
                f.pnames = [re.findall(r"\w+", x)[-1] for x in m.group(1).split(",")]
        todo = []
        for f in funcs.values():
            try:
                ity(f.ret)
                for p in f.params:
                    ity(p)
                todo.append(f)
            except Skip as e:
                f.skip = str(e)
        vecs = {f.name: [[values(p) for p in f.params] for _ in range(24)] for f in todo}
        drv = src + "\n#include <stdio.h>\nint main(void)\n{\n"
        for f in todo:
            for v in vecs[f.name]:
                drv += '    printf("%%lld\\n", (long long)%s(%s));\n' % (
                    f.name, ", ".join(str(x) + ("LL" if abs(x) > 2**31 - 1 else "") for x in v))
        drv += "    return 0;\n}\n"
        with tempfile.TemporaryDirectory() as td:
            c, exe = os.path.join(td, "d.c"), os.path.join(td, "d")
            open(c, "w").write(drv)
            r = subprocess.run(["gcc", "-O0", "-fwrapv", "-w", "-o", exe, c], capture_output=True,
                               text=True, stdin=subprocess.DEVNULL)
            if r.returncode:
                print(path + ": gcc failed\n" + r.stderr[:500])
                bad += 1
                continue
            r = subprocess.run([exe], capture_output=True, text=True, stdin=subprocess.DEVNULL)
            want = r.stdout.split()
        i, ok, skipped = 0, 0, []
        for f in todo:
            res = []
            try:
                for v in vecs[f.name]:
                    got = run(funcs, f, v)
                    res.append(got)
                    exp = int(want[i + len(res) - 1])
                    if got != exp:
                        print("%s: %s%s: got %s, gcc %s" % (path, f.name, tuple(v), got, exp))
                        bad += 1
                        break
                else:
                    ok += 1
            except Skip as e:
                skipped.append("%s (%s)" % (f.name, e))
            i += len(vecs[f.name])
        skipped += ["%s (%s)" % (f.name, f.skip) for f in funcs.values() if f not in todo]
        print("%s: %d functions compared, %d skipped%s" % (
            path, ok, len(skipped), (": " + ", ".join(skipped)) if skipped else ""))
        if ok * 2 < len(funcs):
            print(path + ": fewer than half the functions could be compared")
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
