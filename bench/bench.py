#!/usr/bin/env python3
"""bench.py [--quick] [--only NAME] [CEREAL]

Generates preprocessor workloads under build/bench/ and reports wall time
and peak RSS for `cereal -E`, `gcc -E` and `clang -E` (best of N runs).
Outputs are also compared token-by-token against gcc."""
import os
import random
import resource
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'build', 'bench')


def gen_macro_heavy(path, n):
    """Generated tables whose entries are built from nested macros."""
    with open(path, 'w') as f:
        f.write('#define ADD(a,b) ((a)+(b))\n#define MUL(a,b) ((a)*(b))\n'
                '#define F(x) ADD(MUL(x,3),x)\n')
        for i in range(n):
            f.write(f'static const int t{i}[] = {{ {i}, F({i}), ADD({i},1), '
                    f'0x{i:x}, /* gen */ {i % 7} }};\n')
            if i % 1000 == 0:
                f.write(f'#if {i % 3}\n#define K{i} {i}\n#endif\n')


def gen_table(path, n):
    """Plain data, no macros: pure lexing throughput."""
    rnd = random.Random(1)
    with open(path, 'w') as f:
        f.write('static const unsigned char blob[] = {\n')
        for i in range(n):
            f.write(', '.join(f'0x{rnd.randrange(256):02x}' for _ in range(16)))
            f.write(',\n')
        f.write('};\nstatic const char *names[] = {\n')
        for i in range(n // 4):
            f.write(f'  "identifier_number_{i}_with_some_length", '
                    f"'x', 1.5e{i % 30}f, L\"wide{i}\",\n")
        f.write('};\n')


def gen_comments(path, n):
    """Comment-heavy source (documentation-style generated code)."""
    with open(path, 'w') as f:
        for i in range(n):
            f.write('/*\n * Auto-generated accessor. ' + 'lorem ipsum ' * 8 +
                    '\n */\n')
            f.write(f'int get_{i}(void) {{ return {i}; }} // trailing {i}\n')


def gen_skipped(path, n):
    """Mostly inactive code: exercises the skip scanner."""
    with open(path, 'w') as f:
        for i in range(n):
            f.write(f'#if 0\nint dead_{i}(int x) {{ /* "#if" */ return x * {i}; '
                    f'}}\nconst char *s{i} = "#endif";\n#elif defined(NOPE)\n'
                    f'#else\nint live_{i};\n#endif\n')


def gen_includes(path, n):
    """Many small headers, each included from a main file."""
    d = os.path.join(os.path.dirname(path), 'inc')
    os.makedirs(d, exist_ok=True)
    with open(path, 'w') as f:
        for i in range(n):
            h = os.path.join(d, f'h{i}.h')
            with open(h, 'w') as g:
                g.write(f'#ifndef H{i}\n#define H{i}\n#define V{i} {i}\n'
                        f'int f{i}(int);\n#endif\n')
            f.write(f'#include "inc/h{i}.h"\n#include "inc/h{i}.h"\n')
            f.write(f'int x{i} = V{i};\n')


def gen_sysheaders(path, _n):
    hs = ['assert.h', 'ctype.h', 'errno.h', 'float.h', 'inttypes.h',
          'limits.h', 'math.h', 'signal.h', 'stdarg.h', 'stddef.h',
          'stdint.h', 'stdio.h', 'stdlib.h', 'string.h', 'time.h', 'wchar.h',
          'unistd.h', 'pthread.h', 'sys/socket.h', 'immintrin.h']
    with open(path, 'w') as f:
        for h in hs:
            f.write(f'#include <{h}>\n')


WORKLOADS = [
    ('macro_heavy', gen_macro_heavy, 400000, 40000),
    ('table', gen_table, 600000, 60000),
    ('comments', gen_comments, 300000, 30000),
    ('skipped', gen_skipped, 300000, 30000),
    ('includes', gen_includes, 3000, 500),
    ('sysheaders', gen_sysheaders, 0, 0),
]


def measure(cmd, runs):
    best_t, best_rss = None, 0
    for _ in range(runs):
        t0 = time.perf_counter()
        before = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
        p = subprocess.run(cmd, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
        t = time.perf_counter() - t0
        rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
        if p.returncode not in (0, 1):
            return None, None
        best_t = t if best_t is None else min(best_t, t)
        best_rss = max(best_rss, rss, before)
    return best_t, best_rss


def rss_of(cmd):
    """Peak RSS of one run, isolated in a fresh python child."""
    code = ('import resource,subprocess,sys;'
            'subprocess.run(sys.argv[1:],stdout=subprocess.DEVNULL,'
            'stderr=subprocess.DEVNULL);'
            'print(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)')
    out = subprocess.run([sys.executable, '-c', code, *cmd],
                         capture_output=True, text=True).stdout.strip()
    return int(out) // 1024 if out else 0


def main():
    args = sys.argv[1:]
    quick = '--quick' in args
    only = None
    if '--only' in args:
        only = args[args.index('--only') + 1]
    positional = [a for i, a in enumerate(args)
                  if not a.startswith('--') and (i == 0 or args[i - 1] != '--only')]
    cereal = os.path.abspath(positional[0]) if positional else \
        os.path.join(ROOT, 'cereal')
    os.makedirs(OUT, exist_ok=True)
    runs = 1 if quick else 3
    tools = [('cereal', [cereal, '-E']), ('gcc', ['gcc', '-std=c99', '-E'])]
    if shutil.which('clang'):
        tools.append(('clang', ['clang', '-std=c99', '-E']))
    print(f'{"workload":<12} {"MB":>6} ' +
          ' '.join(f'{n + " s":>9} {n + " MB":>9}' for n, _ in tools) +
          '   vs gcc')
    for name, gen, n_full, n_quick in WORKLOADS:
        if only and name != only:
            continue
        path = os.path.join(OUT, f'{name}.c')
        gen(path, n_quick if quick else n_full)
        size = os.path.getsize(path) / 1e6
        row = f'{name:<12} {size:6.1f} '
        times = {}
        for tname, cmd in tools:
            t, _ = measure(cmd + [path], runs)
            rss = rss_of(cmd + [path])
            times[tname] = t
            row += (f'{t:9.3f} {rss:9d} ' if t is not None
                    else f'{"fail":>9} {"":>9} ')
        if times.get('cereal') and times.get('gcc'):
            row += f'  {times["cereal"] / times["gcc"]:5.2f}x'
        # correctness
        ref = subprocess.run(['gcc', '-std=c99', '-E', path],
                             capture_output=True, text=True).stdout
        mine = subprocess.run([cereal, '-E', path], capture_output=True,
                              text=True).stdout
        with open(os.path.join(OUT, 'ref.i'), 'w') as f:
            f.write(ref)
        with open(os.path.join(OUT, 'out.i'), 'w') as f:
            f.write(mine)
        same = subprocess.run([sys.executable,
                               os.path.join(ROOT, 'tests', 'tokdiff.py'),
                               os.path.join(OUT, 'ref.i'),
                               os.path.join(OUT, 'out.i')],
                              capture_output=True).returncode == 0
        row += '' if same else '  OUTPUT DIFFERS'
        print(row, flush=True)


if __name__ == '__main__':
    main()
