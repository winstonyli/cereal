#!/usr/bin/env python3
"""Layout parity: random struct and union definitions (in the manner of
gcc's testsuite/gcc.dg/compat/struct-layout-1_generate.c), with gcc's
sizeof, _Alignof, __alignof__ and offsetof for each as _Static_assert
checks.  `cereal -fsyntax-only` must accept the file with no diagnostics
(it is not -pedantic clean: _Static_assert, __int128, anonymous members,
empty structs and zero-length arrays are GNU/C11 in C99).

    gen_layout.py --seed N --count K --out file.c [--target x86_64]
                  [--cc 'gcc -m32']

Covered: every scalar type (_Bool .. long double, __int128, _Complex,
pointers, function pointers, enums incl. packed ones), vectors, nested
and anonymous records, arrays (incl. zero-length and flexible), bit-fields
(named, unnamed, zero-width, on enum and aligned-typedef types), packed
and aligned attributes on records, members and typedefs (a typedef may
lower alignment), _Alignas, #pragma pack (N / push, N / pop / ()).

gcc's values come from its assembly for an array initializer (gcc -S), so
a cross target needs only the compiler proper (e.g. gcc -m32 for i386,
aarch64-linux-gnu-gcc).  The same seed gives the same file."""

import argparse
import random
import re
import subprocess
import sys

TARGETS = {
    # name: (compiler command, long bits, pointer bits)
    "x86_64": ("gcc", 64, 64),
    "i386": ("gcc -m32", 32, 32),
    "aarch64": ("aarch64-linux-gnu-gcc", 64, 64),
    "win64": ("x86_64-w64-mingw32-gcc", 32, 64),
}


class Scalar:
    def __init__(self, text, bits=None, bitfield=False, a1=False):
        self.text = text        # type specifier text
        self.bits = bits        # width, if it may carry a bit-field
        self.bitfield = bitfield
        self.a1 = a1            # alignment 1 (packed on it: -Wattributes)


class Gen:
    def __init__(self, rng, target):
        self.r = rng
        _, lbits, pbits = TARGETS[target]
        self.lbits = lbits
        self.n_member = 0
        self.n_type = 0
        self.defs = []          # top-level text blocks, in order
        self.records = []       # (tag text, [paths], flags) of complete records
        self.checks = []        # (expression, label)
        self.enums = []         # Scalar for enum types usable in bit-fields
        self.typedefs = []      # Scalar for typedef names
        self.pack_open = False
        ints = [("_Bool", 1), ("char", 8), ("signed char", 8),
                ("unsigned char", 8), ("short", 16), ("unsigned short", 16),
                ("int", 32), ("unsigned int", 32), ("long", lbits),
                ("unsigned long", lbits), ("long long", 64),
                ("unsigned long long", 64)]
        self.ints = [Scalar(t, b, True, b <= 8) for t, b in ints]
        self.int128 = [Scalar("__int128", 128, True),
                       Scalar("unsigned __int128", 128, True)] \
            if pbits == 64 else []
        self.floats = [Scalar(t, a1=t == "_Complex char") for t in (
            "float", "double", "long double", "_Complex float",
            "_Complex double", "_Complex long double", "_Complex char",
            "_Complex short", "_Complex int", "_Complex long long")]
        self.ptrs = [Scalar(t) for t in ("void *", "char *", "int *",
                                         "double *", "struct opaque *")]

    def name(self):
        self.n_member += 1
        return "m%d" % self.n_member

    def type_name(self, prefix):
        self.n_type += 1
        return "%s%d" % (prefix, self.n_type)

    # ---- types defined at file scope ----------------------------------

    def gen_enum(self):
        r = self.r
        tag = self.type_name("e")
        packed = r.random() < 0.4
        vals = sorted(set(r.choice([0, 1, 2, 3, 7, 100, 127, 128, 255, 256,
                                    300, 32767, 65535, 70000, 0x7fffffff,
                                    -1, -2, -128, -129, -32768])
                          for _ in range(r.randrange(1, 5))))
        if r.random() < 0.1:
            vals.append(r.choice(["0xffffffffu", "0x100000000"]))
        items = ", ".join("%s_%d = %s" % (tag, k, v) for k, v in enumerate(vals))
        attr = " __attribute__((packed))" if packed else ""
        self.defs.append("enum%s %s { %s };" % (attr, tag, items))
        text = "enum " + tag
        self.checks += self.type_checks(text)
        nums = [v for v in vals if isinstance(v, int)]
        if len(nums) == len(vals):
            lo, hi = min(nums), max(nums)
            need = max(1, hi.bit_length() + (1 if lo < 0 else 0),
                       (-lo - 1).bit_length() + 1 if lo < 0 else 0)
            if packed:
                cap = 8 if need <= 8 else 16 if need <= 16 else 32
            else:
                cap = 32
            self.enums.append(Scalar(text, cap, True, cap == 8))
            self.enums[-1].min_bits = need
        else:
            self.enums.append(Scalar(text))

    def gen_typedef(self):
        r = self.r
        name = self.type_name("t")
        k = r.random()
        if k < 0.25:
            base = r.choice(["char", "short", "int", "long long", "double",
                             "float", "long double", "_Complex double"])
            n = r.choice([8, 16, 32]) if base not in ("long double",
                                                        "_Complex double") else 32
            if base in ("char", "short", "int", "float"):
                elem = {"char": 1, "short": 2, "int": 4, "float": 4}[base]
                n = r.choice([s for s in (4, 8, 16, 32) if s >= elem])
                self.defs.append("typedef %s %s __attribute__((vector_size(%d)));"
                                 % (base, name, n))
                self.typedefs.append(Scalar(name))
                self.checks += self.type_checks(name)
                return
            base = "int"
        else:
            s = r.choice(self.ints[1:] + self.floats[:3] + self.int128[:1])
            base = s.text
        al = r.choice([1, 2, 4, 8, 16, 32])
        self.defs.append("typedef %s %s __attribute__((aligned(%d)));"
                         % (base, name, al))
        bits = next((s.bits for s in self.ints + self.int128
                     if s.text == base), None)
        self.typedefs.append(Scalar(name, bits, bits is not None, al == 1))
        self.checks += self.type_checks(name)

    # ---- records ------------------------------------------------------

    def member_type(self, depth):
        """(specifier, declarator format, member paths of a record type,
        plain): plain types have a known alignment > 1 and no attribute,
        so packed, _Alignas and arrays are safe on them."""
        r = self.r
        k = r.random()
        if k < 0.35:
            s = r.choice(self.ints)
        elif k < 0.45:
            s = r.choice(self.floats)
        elif k < 0.5 and self.int128:
            s = r.choice(self.int128)
        elif k < 0.58:
            s = r.choice(self.ptrs)
        elif k < 0.61:
            return "int", "(*%s)(void)", None, True
        elif k < 0.67 and self.enums:
            return r.choice(self.enums).text, "%s", None, False
        elif k < 0.75 and self.typedefs:
            return r.choice(self.typedefs).text, "%s", None, False
        elif k < 0.88 and self.records:
            tag, paths, _ = r.choice(self.records)
            return tag, "%s", paths, False
        else:
            s = r.choice(self.ints)
        plain = not s.a1
        return s.text, "%s", None, plain

    def gen_members(self, depth, is_union, packed, inner_paths, prefix=""):
        """Member lines; appends offsetof designators to inner_paths.
        packed: the record is packed (attribute, not #pragma pack)."""
        r = self.r
        lines = []
        n = r.randrange(0, 7) if depth == 0 else r.randrange(1, 5)
        for _ in range(n):
            k = r.random()
            if k < 0.22:
                lines.append(self.gen_bitfield(packed))
                continue
            if k < 0.29 and depth < 2:
                sub_union = r.random() < 0.4
                sub = []
                attr = self.record_attr_after()
                body = self.gen_members(depth + 1, sub_union, "packed" in attr,
                                        sub)
                lines.append("%s { %s }%s;" % ("union" if sub_union else "struct",
                                               " ".join(body), attr))
                inner_paths += sub   # anonymous: members reachable directly
                continue
            spec, fmt, paths, plain = self.member_type(depth)
            # an array of an over-aligned typedef is an error
            arrays = plain or spec.startswith(("struct", "union", "enum"))
            nm = self.name()
            dims = ""
            dim_n = 0
            if arrays and r.random() < 0.2:
                dim_n = r.choice([1, 2, 3, 5, 0])
                dims = "[%d]" % dim_n
                if dim_n and r.random() < 0.2:
                    dims += "[%d]" % r.choice([1, 2, 3])
            decl = fmt % (nm + dims)
            pre = ""
            if plain and r.random() < 0.03:
                pre = "_Alignas(%d) " % r.choice([16, 32])
            attr = ""
            q = r.random()
            if q < 0.08 and plain:
                attr = " __attribute__((packed))"
            elif q < 0.15:
                attr = " __attribute__((aligned(%d)))" % r.choice([1, 2, 4, 8, 16, 32])
            lines.append("%s%s %s%s;" % (pre, spec, decl, attr))
            path = prefix + nm
            inner_paths.append(path)
            if dims and dim_n >= 2:
                inner_paths.append("%s[%d]" % (path, dim_n - 1))
            if paths and not dims:
                for p in paths[:3]:
                    inner_paths.append(path + "." + p)
        return lines

    def gen_bitfield(self, packed):
        """A packed bit-field of a type of alignment 1 that straddles a
        byte draws gcc's -Wpacked-bitfield-compat note: none such."""
        r = self.r
        pool = self.ints + [e for e in self.enums if e.bitfield] + \
            [t for t in self.typedefs if t.bitfield]
        if r.random() < 0.05 and self.int128:
            pool = self.int128
        s = r.choice(pool)
        attr = ""
        if not s.a1 and r.random() < 0.1:
            attr = " __attribute__((packed))"
        if packed and s.a1:
            s = r.choice(self.ints[4:])
        lo = getattr(s, "min_bits", 1)
        q = r.random()
        if q < 0.15 and s in self.ints + self.int128:
            return "%s : 0;" % s.text
        w = r.randint(lo, s.bits)
        if q < 0.3:
            return "%s : %d%s;" % (s.text, w, attr)
        return "%s %s : %d%s;" % (s.text, self.name(), w, attr)

    def record_attr_after(self):
        r = self.r
        q = r.random()
        if q < 0.1:
            return " __attribute__((packed))"
        if q < 0.17:
            return " __attribute__((aligned(%d)))" % r.choice([1, 2, 4, 8, 16, 32, 64])
        if q < 0.2:
            return " __attribute__((packed, aligned(%d)))" % r.choice([1, 2, 4, 8])
        return ""

    def gen_record(self):
        r = self.r
        is_union = r.random() < 0.25
        kw = "union" if is_union else "struct"
        tag = self.type_name("u" if is_union else "s")
        paths = []
        before = ""
        if r.random() < 0.1:
            before = r.choice([" __attribute__((packed))",
                               " __attribute__((aligned(16)))"])
        after = self.record_attr_after()
        body = self.gen_members(0, is_union, "packed" in before + after, paths)
        fam = False
        if not is_union and paths and r.random() < 0.1:
            fam = True
            nm = self.name()
            body.append("%s %s[];" % (r.choice(["char", "int", "double", "long long"]), nm))
            paths.append(nm)
        text = "%s%s %s { %s }%s;" % (kw, before, tag, "\n    ".join(body),
                                      after)
        pragma_pre, pragma_post = self.pragmas()
        self.defs.append(pragma_pre + text + pragma_post)
        full = "%s %s" % (kw, tag)
        self.checks += self.type_checks(full)
        for p in paths:
            self.checks.append(("__builtin_offsetof(%s, %s)" % (full, p),
                                "offsetof(%s, %s)" % (full, p)))
        if not fam:
            self.records.append((full, paths, None))

    def pragmas(self):
        r = self.r
        q = r.random()
        if self.pack_open:
            if q < 0.4:
                self.pack_open = False
                return "", "\n#pragma pack()"
            return "", ""
        if q < 0.12:
            return "#pragma pack(push, %d)\n" % r.choice([1, 2, 4, 8, 16]), \
                "\n#pragma pack(pop)"
        if q < 0.18:
            self.pack_open = True
            return "#pragma pack(%d)\n" % r.choice([1, 2, 4, 8, 16]), ""
        return "", ""

    def type_checks(self, t):
        return [("sizeof(%s)" % t, "sizeof(%s)" % t),
                ("_Alignof(%s)" % t, "_Alignof(%s)" % t),
                ("__alignof__(%s)" % t, "__alignof__(%s)" % t)]

    def generate(self, count):
        r = self.r
        made = 0
        while made < count:
            k = r.random()
            if k < 0.1:
                self.gen_enum()
            elif k < 0.2:
                self.gen_typedef()
            else:
                self.gen_record()
                made += 1
        if self.pack_open:
            self.defs.append("#pragma pack()")
            self.pack_open = False


def gcc_values(cc, defs, exprs):
    src = "\n".join(defs) + "\nunsigned int layout_values[] = {\n" + \
        ",\n".join("  " + e for e in exprs) + "\n};\n"
    p = subprocess.run(cc.split() + ["-std=c99", "-S", "-o", "-", "-x", "c", "-"],
                       input=src, capture_output=True, text=True)
    if p.returncode != 0 or p.stderr.strip():
        sys.stderr.write(src)
        sys.stderr.write(p.stderr)
        raise SystemExit("gen_layout: %s rejected the probe" % cc)
    vals = []
    inside = False
    for ln in p.stdout.splitlines():
        s = ln.strip()
        if re.match(r"_?layout_values:", s):
            inside = True
            continue
        if not inside:
            continue
        m = re.match(r"\.(long|word|4byte|int)\s+(-?\w+)", s)
        if m:
            vals.append(int(m.group(2), 0))
            continue
        m = re.match(r"\.(zero|space|skip)\s+(\d+)", s)
        if m:
            vals += [0] * (int(m.group(2)) // 4)
            continue
        if s.endswith(":") or s.startswith(".size") or s.startswith(".section") \
                or s.startswith(".text") or s.startswith(".ident"):
            if vals:
                break
    if len(vals) != len(exprs):
        raise SystemExit("gen_layout: read %d values for %d expressions"
                         % (len(vals), len(exprs)))
    return vals


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=20, help="records")
    ap.add_argument("--out", required=True)
    ap.add_argument("--target", default="x86_64", choices=sorted(TARGETS))
    ap.add_argument("--cc", help="the compiler for the target "
                    "(default: gcc, gcc -m32, <triple>-gcc)")
    a = ap.parse_args()
    g = Gen(random.Random(a.seed), a.target)
    g.generate(a.count)
    cc = a.cc or TARGETS[a.target][0]
    vals = gcc_values(cc, g.defs, [e for e, _ in g.checks])
    with open(a.out, "w") as f:
        f.write("// generated by tests/gen_layout.py --seed %d --count %d "
                "--target %s\n" % (a.seed, a.count, a.target))
        f.write("// flags: --target=%s\n" % a.target)
        f.write("\n".join(g.defs) + "\n\n")
        for (e, label), v in zip(g.checks, vals):
            f.write('_Static_assert(%s == %d, "%s");\n'
                    % (e, v, label.replace("\\", "\\\\").replace('"', '\\"')))


if __name__ == "__main__":
    main()
