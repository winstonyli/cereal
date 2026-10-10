#!/usr/bin/env python3
"""verify.py CEREAL ARGS... FILE - check diagnostics against annotations.

In the source, a comment `expect: ID[, ID...]` on a line means cereal must
report a diagnostic with that option id (or `error` for hard errors) on that
line.  `expect+N: ID` targets N lines below.  Any diagnostic in the main file
that is not expected is a failure, except notes.

`expect-fixit: L:C-L:C "text" APP` (several per line are fine) names a fix-it:
the range from L:C up to L:C (the same pair for an insertion), the replacement
as a JSON string, and its applicability (safe, maybe or info).  When a file has
any, the fix-its cereal gives for the main file (on diagnostics and on notes)
must be exactly these."""
import json
import re
import subprocess
import sys

EXPECT = re.compile(r'expect(?:([+-]\d+))?:\s*([\w\-, ]+)')
FIXIT = re.compile(r'expect-fixit:\s*(\d+):(\d+)-(\d+):(\d+)\s+("(?:[^"\\]|\\.)*")'
                   r'\s+(safe|maybe|info)\b', re.I)


def fixits_of(d, path):
    """Every fix-it of a diagnostic and its notes in the file `path`."""
    out = []
    for x in [d, *d.get('notes', [])]:
        for f in x.get('fixits', []):
            s, e = f['start'], f['next']
            if s['file'] == path or s['file'].endswith('/' + path):
                out.append((s['line'], s['col'], e['line'], e['col'],
                            f['string'], f['applicability'].lower()))
    return out


def main():
    cereal, args, path = sys.argv[1], sys.argv[2:-1], sys.argv[-1]
    want = {}
    wantfix = []
    flags = []
    with open(path, encoding='utf-8') as f:
        for n, line in enumerate(f, 1):
            fm = re.search(r'cereal-flags:\s*(.*?)\s*(\*/|$)', line)
            if fm:
                flags += fm.group(1).split()
            for m in FIXIT.finditer(line):
                wantfix.append((*map(int, m.group(1, 2, 3, 4)),
                                json.loads(m.group(5)), m.group(6).lower()))
            for m in EXPECT.finditer(line):
                off = int(m.group(1) or 0)
                for i in m.group(2).split(','):
                    i = i.strip()
                    if i:
                        want.setdefault((n + off, i), 0)
                        want[(n + off, i)] += 1
    p = subprocess.run([cereal, *args, *flags, '-fdiagnostics-format=json', path],
                       capture_output=True, text=True)
    out = p.stdout
    if args and args[0] == '-E':
        # JSON diagnostics follow the preprocessed text on stdout
        out = out[out.rfind('\n[') + 1:] if '\n[' in out else out[out.find('['):]
    try:
        diags = json.loads(out)
    except ValueError:
        print('could not parse cereal output:\n' + p.stdout[-2000:] + p.stderr)
        return 1
    got = {}
    gotfix = []
    for d in diags:
        gotfix += fixits_of(d, path)
        if d['file'] != path and not d['file'].endswith('/' + path):
            continue
        key = (d['line'], d['id'] or 'error')
        got.setdefault(key, []).append(d['message'])
    rc = 0
    for key, cnt in sorted(want.items()):
        if key not in got:
            print(f'{path}:{key[0]}: expected [{key[1]}] not reported')
            rc = 1
    for key, msgs in sorted(got.items()):
        if key not in want:
            print(f'{path}:{key[0]}: unexpected [{key[1]}]: {msgs[0]}')
            rc = 1
    if wantfix:
        for fx in sorted(set(wantfix) - set(gotfix)):
            print(f'{path}:{fx[0]}: expected fix-it {fx[:4]} {fx[4]!r} {fx[5]} not reported')
            rc = 1
        for fx in sorted(set(gotfix) - set(wantfix)):
            print(f'{path}:{fx[0]}: unexpected fix-it {fx[:4]} {fx[4]!r} {fx[5]}')
            rc = 1
    if p.returncode not in (0, 1):
        print(f'cereal exited with {p.returncode}\n{p.stderr[-2000:]}')
        rc = 1
    return rc


sys.exit(main())
