#!/usr/bin/env python3
"""Throwaway (AR1 Task 1): diff gen_builtin_attrs.py output (argv[1]) against
the old side tables.  Deleted in Task 5."""
import re, sys
gen = {}
for l in open(sys.argv[1]):
    n, p, m, f = l.split()
    gen[n] = (int(p), int(m), f)
cc = open('src/c/ccall.c').read()
blk = cc[cc.index('static uint64_t builtin_nonnull'):]
blk = blk[:blk.index('size_t k;')]
oldnn = {n: sum(1 << (int(d) - 1) for d in p) for n, p in re.findall(r'\{"(\w+)", "(\d+)"\}', blk)}
cf = open('src/c/cformat.c').read()
oldf = {n: '%d:printf' % int(p) for n, p in re.findall(r'\{"(\w+)", (\d)\}', cf[:cf.index('-Wformat: the conv')])}
for n, p in (('scanf', 1), ('vscanf', 1), ('fscanf', 2), ('sscanf', 2), ('vfscanf', 2), ('vsscanf', 2)):
    oldf[n] = '%d:scanf' % p
oldf['strfmon'] = '3:strfmon'
oldf['strftime'] = '3:strftime'
oldp = set(re.findall(r'"(\w+)"', open('src/c/cbuiltin_pure.h').read()))
d = 0
def show(k, n, o, g):
    global d
    d += 1
    print('%-8s %-22s old=%-10s new=%s' % (k, n, o, g))
for n in sorted(set(gen) | oldp | set(oldnn) | set(oldf)):
    g = gen.get(n)
    if g is None:
        show('absent', n, '(not a tab row)', ''); continue
    if (n in oldp) != bool(g[0]): show('pure', n, n in oldp, g[0])
    if oldnn.get(n, 0) != g[1]: show('nonnull', n, oldnn.get(n, 0), g[1])
    if oldf.get(n, '0') != g[2]: show('fmt', n, oldf.get(n, '0'), g[2])
print('differences', d)
