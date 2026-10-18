#!/usr/bin/env python3
"""tests/tast_data.py - static data regression for --dump-tast (T3 plan, T3b).

Evaluates every `data` node of cereal's dump into bytes plus relocations and
compares them with what gcc-13 -O0 -c puts in the object file for the same
object: the same bytes, and the same relocations (offset, target symbol or
string contents, addend).

usage: tests/tast_data.py CEREAL FILE.c...        env: GCC (default gcc-13), TAST_FLAGS
"""
import os
import re
import subprocess
import sys
import tempfile


FLAGS = os.environ.get("TAST_FLAGS", "").split()     # extra compiler flags (-I, -D) for both


def run(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True, stdin=subprocess.DEVNULL,
                          check=True).stdout


def base_name(n):
    return re.sub(r"\.\d+$", "", n)


# ---- gcc side ---------------------------------------------------------------

def gcc_objects(gcc, path, d):
    obj = os.path.join(d, "o.o")
    run(gcc, "-std=gnu11", "-w", "-O0", "-fno-pic", "-fno-pie", "-fno-common", *FLAGS, "-c", path, "-o", obj)
    secs = {}
    for m in re.finditer(r"\[\s*(\d+)\]\s+(\S+)\s+(\S+)", run("readelf", "-SW", obj)):
        secs[int(m.group(1))] = (m.group(2), m.group(3))
    data = {}
    for idx, (name, typ) in secs.items():
        if typ != "PROGBITS":
            continue
        buf = bytearray()
        for line in run("objdump", "-s", "-j", name, obj).splitlines():
            m = re.match(r"^ ([0-9a-f]+) ((?:[0-9a-f]+ ?){1,4})", line)
            if m:
                at = int(m.group(1), 16)
                b = bytes.fromhex(m.group(2).replace(" ", ""))
                buf[at:at + len(b)] = b
        data[name] = bytes(buf)
    relocs = {}
    cur = None
    for line in run("readelf", "-rW", obj).splitlines():
        m = re.match(r"Relocation section '\.rela(\S+)'", line)
        if m:
            cur = m.group(1)
            continue
        m = re.match(r"^([0-9a-f]{8,16})\s+[0-9a-f]+\s+(R_\w+)\s+[0-9a-f]+\s+(\S+) ([+-]) ([0-9a-f]+)", line)
        if m and cur:
            add = int(m.group(5), 16) * (1 if m.group(4) == "+" else -1)
            relocs.setdefault(cur, []).append((int(m.group(1), 16), m.group(2), m.group(3), add))
    found = []
    for line in run("readelf", "-sW", obj).splitlines():
        f = line.split()
        if len(f) >= 8 and f[3] in ("OBJECT", "FUNC") and f[6].isdigit() and int(f[6]) in secs:
            found.append((secs[int(f[6])][0], int(f[1], 16), int(f[2]), base_name(f[7]), f[3], f[7]))
    objs = {}
    for sec, at, size, name, kind, raw in found:
        if kind != "OBJECT":
            continue
        buf = data.get(sec, bytes(at + size))[at:at + size]
        rl = []
        for off, typ, sym, add in relocs.get(sec, []):
            if not at <= off < at + size:
                continue
            if sym.startswith("."):      # a section symbol: a static object, else a string
                hit = [(n, a) for s2, a, z, n, _, _r in found if s2 == sym and a <= add < a + z]
                if hit:
                    rl.append((off - at, ("sym", hit[0][0]), add - hit[0][1]))
                    continue
                s = data[sym][add:]
                rl.append((off - at, ("str", s[:s.index(0) + 1] if 0 in s else s), 0))
            else:
                rl.append((off - at, ("sym", base_name(sym)), add))
        n = re.search(r"\.(\d+)$", raw)      # gcc numbers equal-named statics .0, .1 ...
        objs.setdefault(name, []).append((int(n.group(1)) if n else -1, (buf, sorted(rl))))
    return {n: [v for _, v in sorted(l, key=lambda p: p[0])] for n, l in objs.items()}


# ---- cereal side ------------------------------------------------------------

class Unsup(Exception):
    pass


def tsize(t):
    """Size in bytes of a printed scalar or array type."""
    if "(*" in t:               # pointer to array or function
        return 8
    m = re.match(r"^(.*?)((?:\[\d+\])+)$", t)
    if m:
        n = 1
        for k in re.findall(r"\[(\d+)\]", m.group(2)):
            n *= int(k)
        return n * tsize(m.group(1))
    if "*" in t:
        return 8
    if "long" in t:
        return 8
    if "short" in t:
        return 2
    if "char" in t or "_Bool" in t:
        return 1
    if "int" in t or "enum" in t:
        return 4
    raise Unsup("type " + t)


def trunc(v, t):
    bits = 8 * tsize(t)
    v &= (1 << bits) - 1
    if "unsigned" not in t and "*" not in t and "_Bool" not in t and v >> (bits - 1):
        v -= 1 << bits
    return v


class Unit:
    def __init__(self):
        self.n = {}

    def add(self, line):
        m = re.match(r"^\s+#(\d+) (\w+)(.*)$", line)
        kids = re.findall(r"#(\d+)", m.group(3))
        # attributes: tokens before the first kid, the type is what follows the last kid
        rest = m.group(3).strip()
        self.n[int(m.group(1))] = (m.group(2), rest, [int(k) for k in kids])

    def ty(self, rest, kids):
        if kids:
            return rest.rsplit("#%d" % kids[-1], 1)[1].strip()
        return rest.split(None, 1)[1] if " " in rest else ""

    def ev(self, i):
        op, rest, kids = self.n[i]
        toks = rest.split()
        if op == "const":
            return ("i", int(toks[0]))
        if op == "sym":
            return ("lv", ("sym", base_name(re.sub(r"@.*", "", toks[0]))), 0)
        if op == "str":
            return ("lv", ("str", bytes.fromhex(toks[1])), 0)
        if op == "cast":
            v = self.ev(kids[0])
            t = self.ty(rest, kids)
            return ("i", trunc(v[1], t)) if v[0] == "i" else v
        if op == "addr":
            v = self.ev(kids[0])
            if v[0] != "lv":
                raise Unsup("addr of a value")
            return ("a", v[1], v[2])
        if op == "ptradd":
            p, x = self.ev(kids[0]), self.ev(kids[1])
            return ("a", p[1], p[2] + x[1] * int(toks[0][1:]))
        if op == "deref":
            p = self.ev(kids[0])
            return ("lv", p[1], p[2])
        if op == "field":
            v = self.ev(kids[0])
            return ("lv", v[1], v[2] + int(toks[0][1:]))
        if op == "unop" and toks[0] in ("-", "~", "!"):
            v = self.ev(kids[0])[1]
            return ("i", {"-": -v, "~": ~v, "!": int(not v)}[toks[0]])
        if op == "binop" and toks[0] in ("+", "-", "*", "|", "&", "^", "<<", ">>"):
            a, b = self.ev(kids[0])[1], self.ev(kids[1])[1]
            return ("i", trunc({"+": a + b, "-": a - b, "*": a * b, "|": a | b, "&": a & b, "^": a ^ b,
                                "<<": a << b, ">>": a >> b}[toks[0]], self.ty(rest, kids)))
        raise Unsup(op)

    def data(self, i):
        op, rest, kids = self.n[i]
        name = base_name(re.sub(r"@.*", "", rest.split()[0]))
        elems = []
        for k in kids:
            eop, erest, ekids = self.n[k]
            m = re.match(r"\+(\d+)(?: bit (\d+) width (\d+))? #\d+ (.*)$", erest)
            elems.append((int(m.group(1)), m.group(2), m.group(3), ekids[0], m.group(4)))
        return name, elems

    def image(self, i, size):
        name, elems = self.data(i)
        buf, rl = bytearray(size), []
        for off, bit, width, kid, t in elems:
            if t.endswith("]") and self.n[kid][0] == "str":
                unit = int(self.n[kid][1].split()[0][1:])
                s = bytes.fromhex(self.n[kid][1].split()[1])
                s = s[:tsize(t)]
                buf[off:off + len(s)] = s
                continue
            v = self.ev(kid)
            if width:
                w, b = int(width), int(bit)
                cur = int.from_bytes(buf[off:off + 8], "little")
                cur &= ~(((1 << w) - 1) << b)
                cur |= (v[1] & ((1 << w) - 1)) << b
                n = (b + w + 7) // 8
                buf[off:off + n] = (cur & ((1 << (8 * n)) - 1)).to_bytes(n, "little")
            elif v[0] == "i":
                n = tsize(t)
                buf[off:off + n] = (v[1] & ((1 << (8 * n)) - 1)).to_bytes(n, "little")
            else:
                if tsize(t) != 8:
                    raise Unsup("address in a narrow object")
                tgt = v[1]
                if tgt[0] == "str":      # gcc points into the string: addend 0, contents compared
                    s = tgt[1][v[2]:]
                    tgt, add = ("str", s[:s.index(0) + 1] if 0 in s else s), 0
                else:
                    add = v[2]
                rl.append((off, tgt, add))
        return name, bytes(buf), sorted(rl)


def cereal_objects(cereal, path):
    out = subprocess.run([cereal, "check", "--target=x86_64-linux-gnu", "--verify-tast", *FLAGS, path],
                         capture_output=True, text=True, stdin=subprocess.DEVNULL).stdout
    bad = [l for l in out.splitlines() if l.startswith("verify:")]
    units, cur = [], None
    for line in out.splitlines():
        if line.startswith("unit "):
            cur = Unit()
            units.append(cur)
        elif line.startswith("  #") and cur is not None:
            cur.add(line)
    return units, bad, out


def main():
    cereal, files = sys.argv[1], sys.argv[2:]
    gcc = os.environ.get("GCC", "gcc-13")
    fails = checked = skipped = 0
    for path in files:
        try:
            with tempfile.TemporaryDirectory() as d:
                objs = gcc_objects(gcc, path, d)
        except subprocess.CalledProcessError:
            print("%s: gcc failed, skipped" % path)
            continue
        units, bad, _ = cereal_objects(cereal, path)
        pending = {n: list(v) for n, v in objs.items()}     # equal names (gcc: x.0, x.1) match in any order
        for b in bad:
            print("%s: %s" % (path, b))
            fails += 1
        for u in units:
            for i, (op, rest, kids) in sorted(u.n.items()):
                if op != "data":
                    continue
                name = base_name(re.sub(r"@.*", "", rest.split()[0]))
                if not pending.get(name):
                    print("%s: %s: not in the gcc object" % (path, name))
                    fails += 1
                    continue
                try:
                    got = [(u.image(i, len(w[0]))[1:], w) for w in pending[name]]
                except Unsup as e:
                    print("%s: %s: skipped (%s)" % (path, name, e))
                    skipped += 1
                    pending[name].pop(0)
                    continue
                checked += 1
                hit = [w for g, w in got if g == w]
                if hit:
                    pending[name].remove(hit[0])
                else:
                    g, w = got[0]
                    print("%s: %s differs\n  cereal %s %s\n  gcc    %s %s" %
                          (path, name, g[0].hex()[:96], g[1][:6], w[0].hex()[:96], w[1][:6]))
                    fails += 1
                    pending[name].pop(0)
    print("%d objects compared, %d skipped, %d failures" % (checked, skipped, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
