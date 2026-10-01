# Full-diagnostic (warnings included) parity over a par.py dg result: python3 diagstat.py dg_out.json
import json, re, collections

import sys
r = json.load(open(sys.argv[1]))


def L(x):
    return [tuple(y) for y in (json.loads(x) if isinstance(x, str) else x)]


def norm(t):
    return re.sub(r"'[^']*'|‘[^’]*’", "'X'", t).strip()


nfile = 0
exact = 0
missing = collections.Counter()
extra = collections.Counter()
mfiles = collections.defaultdict(set)
efiles = collections.defaultdict(set)
for v in r:
    g = L(v['g'])
    c = L(v['c'])
    nfile += 1
    gs = collections.Counter(g)
    cs = collections.Counter(c)
    if gs == cs:
        exact += 1
        continue
    fn = v['f'].split('/')[-1]
    for x, n in (gs - cs).items():
        k = (x[2], norm(x[3]))
        missing[k] += n
        mfiles[k].add(fn)
    for x, n in (cs - gs).items():
        k = (x[2], norm(x[3]))
        extra[k] += n
        efiles[k].add(fn)
print('files', nfile, 'identical diagnostics', exact, 'differ', nfile - exact)
print('missing total', sum(missing.values()), 'distinct', len(missing))
print('extra total', sum(extra.values()), 'distinct', len(extra))
print('-- top missing (by files)')
for k, n in sorted(missing.items(), key=lambda kv: -len(mfiles[kv[0]]))[:40]:
    print(len(mfiles[k]), n, k[0], k[1][:100])
print('-- top extra (by files)')
for k, n in sorted(extra.items(), key=lambda kv: -len(efiles[kv[0]]))[:25]:
    print(len(efiles[k]), n, k[0], k[1][:100])
