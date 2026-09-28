#!/usr/bin/env python3
"""tokdiff.py A B - compare two preprocessed outputs token by token.

Linemarkers and blank lines are ignored; whitespace only matters where it
separates tokens. Exit 0 if identical."""
import re
import sys

TOKEN = re.compile(r'''
    L?"(?:\\.|[^"\\\n])*"      |
    L?'(?:\\.|[^'\\\n])*'      |
    \.?[0-9](?:[eEpP][+-]|[0-9A-Za-z_.])*  |
    [A-Za-z_$][A-Za-z0-9_$]*  |
    %:%:|\.\.\.|<<=|>>=|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||[*/%+\-&^|]=|
    \#\#|<:|:>|<%|%>|%:|
    \S
''', re.X)


def tokens(path):
    out = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for lineno, line in enumerate(f, 1):
            if re.match(r'^\s*#\s*\d+\s+"', line):
                continue  # linemarker
            if re.match(r'^\s*#\s*pragma\b', line):
                out.append(('#pragma', lineno))
                continue
            for t in TOKEN.findall(line):
                out.append((t, lineno))
    return out


def main():
    a, b = tokens(sys.argv[1]), tokens(sys.argv[2])
    for i, (x, y) in enumerate(zip(a, b)):
        if x[0] != y[0]:
            ctx_a = ' '.join(t for t, _ in a[max(0, i - 8):i + 8])
            ctx_b = ' '.join(t for t, _ in b[max(0, i - 8):i + 8])
            print(f'token {i}: {x[0]!r} ({sys.argv[1]}:{x[1]}) != '
                  f'{y[0]!r} ({sys.argv[2]}:{y[1]})')
            print(f'  A: {ctx_a}\n  B: {ctx_b}')
            return 1
    if len(a) != len(b):
        print(f'length differs: {len(a)} vs {len(b)}')
        return 1
    return 0


sys.exit(main())
