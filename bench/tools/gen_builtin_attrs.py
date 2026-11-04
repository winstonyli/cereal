#!/usr/bin/env python3
"""Probe gcc-13 for the per-name columns of src/c/cbuiltin_tab.h and print
"NAME pure nonnull fmt" for every row (nonnull: bitmask of 1-based argument
positions; fmt: "0" or "pos:kind", kind printf|scanf|strfmon|strftime).
Run from the repo root (WSL).  See docs/BUILTIN_TABLE_DESIGN.md.

  pure     a call whose unused result warns "statement with no effect"
  nonnull  a null literal at a pointer position warns "argument N null where
           non-null expected"
  fmt      the position where the unknown conversion "%Q" is diagnosed; kind
           scanf when "%[a]" is accepted there (printf rejects it)
Names whose probe does not compile (types the includes do not provide) get
0 and are listed on stderr."""
import re, subprocess, sys, tempfile, os

ROW = re.compile(r'^    \{"(\w+)", "[^"]*", \d, "([^"]*)", \d(?:, \d+)*\},$', re.M)
HEAD = ('#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n'
        '#include <time.h>\n#include <wchar.h>\n#include <monetary.h>\n'
        'char gp[64]; int gi; double gd; __builtin_va_list gv;\n')
NHEAD = HEAD.count('\n')


def gcc(src, flags):
    d = tempfile.mkdtemp()
    p = os.path.join(d, 'probe.c')
    open(p, 'w').write(src)
    r = subprocess.run(['gcc-13', '-fsyntax-only'] + flags + [p],
                       capture_output=True, text=True,
                       env=dict(os.environ, LC_ALL='C'))
    return r.stderr


def arg(t, null=False):
    if t == '__va_list_tag *':
        return 'gv'
    if '*' in t:
        return '(%s)%s' % (t, '0' if null else 'gp')
    if 'double' in t or 'float' in t:
        return '(%s)gd' % t
    return '(%s)gi' % t


def rows():
    src = open('src/c/cbuiltin_tab.h').read()
    return [(n, s.split('|')) for n, s in ROW.findall(src)]


def probe(calls, flags, pat):
    """calls: [(key, call text)]; one function per line; returns the keys
    whose line drew a warning matching pat."""
    body = ''.join('void f%d(void){ %s; }\n' % (i, c) for i, (_, c) in enumerate(calls))
    err = gcc(HEAD + body, flags)
    hit = set()
    for m in re.finditer(r'probe\.c:(\d+):\d+: warning: ' + pat, err):
        i = int(m.group(1)) - NHEAD - 1
        if 0 <= i < len(calls):
            hit.add(calls[i][0])
    return hit


def compute():
    """name -> (pure, nonnull mask, fmt byte)"""
    rs = rows()
    pure_calls, nn_calls, fmt_calls, kind_calls = [], [], [], []
    for n, sig in rs:
        ret, params = sig[0], [p for p in sig[1:] if p]
        if '?' in sig:
            continue
        fixed = [p for p in params if p != '...']
        variadic = '...' in params
        if not variadic:
            pure_calls.append((n, '__builtin_%s(%s)' % (n, ', '.join(arg(p) for p in params))))
        for k, p in enumerate(fixed):
            if '*' in p and p != '__va_list_tag *':
                a = [arg(q) for q in fixed]
                a[k] = arg(p, True)
                nn_calls.append(((n, k + 1), '__builtin_%s(%s)' % (n, ', '.join(a))))
            if p in ('const char *', 'const char * restrict'):
                a = [arg(q) for q in fixed]
                a[k] = '"%Q"'
                fmt_calls.append(((n, k + 1), '__builtin_%s(%s)' % (n, ', '.join(a))))
                a[k] = '"%[a]"'
                kind_calls.append(((n, k + 1), '__builtin_%s(%s)' % (n, ', '.join(a))))
    pure = probe(pure_calls, ['-Wunused-value'], r'(statement with no effect|value computed is not used)')
    nn = probe(nn_calls, ['-Wnonnull'], r'argument \d+ null where non-null expected')
    bad = probe(fmt_calls, ['-Wformat'], r"unknown conversion type character .Q. in format")
    notscan = probe(kind_calls, ['-Wformat'], r"unknown conversion type character .\[. in format")
    out = {}
    for n, sig in rs:
        mask = 0
        for (m, k) in nn:
            if m == n:
                mask |= 1 << (k - 1)
        pos = sorted(k for (m, k) in bad if m == n)
        f = 0
        if pos:
            kind = (3 if n == 'strftime' else 2 if n == 'strfmon'
                    else 0 if (n, pos[0]) in notscan else 1)
            f = pos[0] | kind << 4
        out[n] = (int(n in pure), mask, f)
    return out


FULL = re.compile(r'^    \{"(\w+)", ("[^"]*"), (\d), ("[^"]*"), (\d)(?:, \d+)*\},$', re.M)


def main():
    attrs = compute()
    if '--write' in sys.argv:
        path = 'src/c/cbuiltin_tab.h'
        src = open(path, encoding='utf-8').read()
        src = FULL.sub(lambda m: '    {"%s", %s, %s, %s, %s, %d, %d, %d},' % (
            (m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)) + attrs[m.group(1)]), src)
        open(path, 'w', encoding='utf-8').write(src)
        return
    for n, (p, m, f) in attrs.items():
        k = ['printf', 'scanf', 'strfmon', 'strftime'][f >> 4]
        print(n, p, m, '%d:%s' % (f & 15, k) if f else '0')


if __name__ == '__main__':
    main()
