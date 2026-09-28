"""Random preprocessor programs for the fuzzers (fuzz_par.py, fuzz_gcc.py).

Programs are built to stress the edges: invocations spanning lines,
directives and include ends; #line; _Pragma and #pragma in arguments;
redefinition inside arguments; # and ## with odd operands; variadics and
empty arguments; conditionals on macro values; splices and comments."""

NAMES = ["A", "B", "F", "G", "H", "E", "X", "V", "AB", "X1"]
PARAMS = ["", "x", "x, y", "x, ...", "...", "a, b, c"]


def body(r):
    parts = []
    for _ in range(r.randrange(0, 6)):
        parts.append(r.choice([
            "x", "y", "a", "b", "c", "__VA_ARGS__", "#x", "x ## y", "a ## 1",
            "A ## B", "X ## 1", "x ## B", "X ## y",
            "## x", "x ##", "(", ")", ",", "+", "1", "L", "\"s\"", "'c'",
            r.choice(NAMES), r.choice(NAMES) + "(", "__LINE__", "__FILE__",
            "_Pragma(\"p\")", "#", "defined", ".", "..."]))
    return " ".join(parts)


def args(r, depth=0):
    out = []
    for _ in range(r.randrange(0, 4)):
        k = r.randrange(8)
        if k == 0 and depth < 2:
            out.append("%s(%s)" % (r.choice(NAMES), args(r, depth + 1)))
        elif k == 1 and depth < 2:
            out.append("(%s)" % args(r, depth + 1))
        elif k == 2:
            out.append("")
        else:
            out.append(r.choice(NAMES + ["1", "x", "+", "\"a,b\"", "'('", "a##b"]))
    return ", ".join(out)


def line(r, depth):
    n = r.choice(NAMES)
    k = r.randrange(34)
    if k < 3:
        return "#define %s(%s) %s" % (n, r.choice(PARAMS), body(r))
    if k < 5:
        return "#define %s %s" % (n, body(r))
    if k == 5:
        return "#undef %s" % n
    if k == 6:
        cond = r.choice(["1", "0", "defined(%s)" % n, "%s + 1" % n,
                         "%s(1) == 1" % n, "defined %s || %s" % (n, n),
                         "__LINE__ > 3", "(%s)" % n])
        return "#if %s\n%s\n#elif %s\n%s\n#else\n%s\n#endif" % (
            cond, line(r, depth), r.choice(["1", "0", n]), line(r, depth),
            line(r, depth))
    if k == 7:
        return "#line %d%s" % (r.randrange(1, 500),
                               r.choice(["", ' "fake.c"', ' "o.h"', " x", ' "a" b']))
    if k == 8 and depth < 2:
        return '#include "inc%d.h"' % r.randrange(3)
    if k == 9:
        return r.choice(["_Pragma(\"omp parallel\") x", "_Pragma(", "_Pragma(\"once\")",
                         "#pragma once_not %d" % r.randrange(9), "#pragma GCC poison Q"])
    if k == 10:
        return "/* multi\n   line %d */ y" % r.randrange(9)
    if k == 11:
        return "%s(" % n
    if k == 12:
        return ")"
    if k == 13:
        return r.choice(["", "   \t  ", "#", "# 12 \"m.c\"", "#ident \"x\"",
                         "" if r.randrange(8) else "__COUNTER__"])
    if k == 14:
        return "s = \"str %d\"; // c" % r.randrange(9)
    if k == 15:
        return r.choice(["long \\\n  splice;", "a\\\nb", "\"x\\\ny\""])
    if k == 16:
        return "#ifdef %s\n%s(%s)\n#endif" % (n, n, args(r))
    if k == 17:
        return "#ifndef %s\n#define %s\n%s\n#endif" % (n, n, line(r, depth))
    if k < 26:
        return "%s(%s)%s" % (n, args(r), r.choice(["", " x", ";", "(1)"]))
    return " ".join(r.choice(NAMES + ["(", ")", ",", "+", "1", "x", "a##b", "#",
                                      "__LINE__", "%s()" % n])
                    for _ in range(r.randrange(1, 8)))


def gen_file(r, lines, depth):
    return "\n".join(line(r, depth) for _ in range(lines)) + "\n"


def gen_program(r, d):
    """Write main.c and inc0..2.h into directory d."""
    import os
    for i in range(3):
        with open(os.path.join(d, "inc%d.h" % i), "w") as f:
            f.write(gen_file(r, r.randrange(1, 12), 2))
    with open(os.path.join(d, "main.c"), "w") as f:
        f.write(gen_file(r, r.randrange(5, 80), 0))
