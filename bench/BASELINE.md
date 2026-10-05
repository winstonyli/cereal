# Baseline (before token-core rewrite)

Commit 77cd649-era core (eager linked-list tokens, Prosser hide sets). Best of
3 runs, `python3 bench/bench.py`.

| workload | MB | cereal s | cereal MB | gcc s | gcc MB | clang s | clang MB | vs gcc |
|---|---|---|---|---|---|---|---|---|
| macro_heavy | 35.1 | 8.090 | 3542 | 3.071 | 527 | 3.511 | 284 | 2.63x |
| table | 68.7 | 4.811 | 1316 | 3.224 | 602 | 0.965 | 142 | 1.49x |
| comments | 57.0 | 0.905 | 271 | 0.640 | 211 | 0.296 | 153 | 1.41x |
| skipped | 43.4 | 2.969 | 1108 | 2.115 | 605 | 0.498 | 165 | 1.40x |
| includes | 0.2 | 0.101 | 12 | 0.029 | 13 | 0.056 | 76 | 3.52x |
| sysheaders | 0.0 | 0.114 | 47 | 0.053 | 15 | 0.065 | 80 | 2.16x |

# After the token-core rewrite (stages 2–5)

Best of 3, same machine. The lexer uses SSE2 run scanners (the scalar build
`-DCEREAL_NO_SIMD` is 0–5% slower: runs in real C are short, and lexing is
about a third of the time).

| workload | MB | cereal s | cereal MB | gcc s | gcc MB | clang s | clang MB | vs gcc |
|---|---|---|---|---|---|---|---|---|
| macro_heavy | 35.1 | 1.187 | 72 | 3.037 | 527 | 3.558 | 284 | 0.39x |
| table | 68.7 | 0.611 | 68 | 3.288 | 602 | 1.063 | 142 | 0.19x |
| comments | 57.0 | 0.234 | 87 | 0.697 | 211 | 0.311 | 153 | 0.34x |
| skipped | 43.4 | 0.420 | 82 | 2.595 | 605 | 0.523 | 165 | 0.16x |
| includes | 0.2 | 0.031 | 18 | 0.032 | 13 | 0.061 | 76 | 0.96x |
| sysheaders | 0.0 | 0.031 | 9 | 0.054 | 15 | 0.068 | 80 | 0.57x |

Memory is now dominated by the mapped input file.
On macro_heavy: `lint` takes 1.17 s / 158 MB, `index` 4.24 s / 435 MB (eager
per-expansion argument and result text; to be made lazy).

# Parallel `-E` (two-phase, 4 cores)

Best of 3. `cereal` uses the default `-fparallel=auto`; `seq` is `-fparallel=off`.
Auto goes parallel only for main files of at least 4 MB, so the last two rows
are sequential.

| workload | MB | cereal s | cereal MB | seq s | seq MB | gcc s | clang s | vs gcc |
|---|---|---|---|---|---|---|---|---|
| macro_heavy | 35.1 | 0.457 | 116 | 1.195 | 80 | 2.822 | 3.240 | 0.16x |
| table | 68.7 | 0.259 | 133 | 0.741 | 68 | 3.245 | 0.969 | 0.08x |
| comments | 57.0 | 0.214 | 105 | 0.239 | 94 | 0.683 | 0.296 | 0.31x |
| skipped | 43.4 | 0.496 | 142 | 0.446 | 90 | 2.342 | 0.463 | 0.21x |
| includes | 0.2 | 0.052 | 19 | 0.049 | 19 | 0.029 | 0.057 | 1.77x |
| sysheaders | 0.0 | 0.030 | 10 | 0.030 | 10 | 0.054 | 0.071 | 0.56x |

Speedup over sequential: 2.6x on macro_heavy and 2.9x on table. `comments`
is limited by memory bandwidth. `skipped` is directive-dense (1.5M plan
items for 5 MB of text), so sequential phase A takes 0.35 s of it; that
case needs the plan-density heuristic or pipelined phases. Output is held
in memory until the merge (the extra MB).

# Real code (bench/corpus.py)

`bench/fetch_corpus.sh DIR` downloads source distributions from PyPI that
carry real C; `bench/corpus.py ./cereal DIR` checks every translation unit
(Lua 5.1-5.5 with `-DLUA_USE_LINUX`, libuv's portable and Linux sources,
zstd as one file, and the Cython-generated `lupa/lua54.c` and
`uvloop/loop.c`): token-equal to `gcc -E`; forced parallel `-E`, `lint`
and `index` equal to sequential; `index --replay` with cells, rebuilt
after a line is inserted mid-file, equal to a sequential rebuild.

All 250 units pass. `-E` over the whole corpus: gcc 4.3 s, cereal 2.1 s.

| unit | MB | gcc -E s | cereal seq s | cereal par s |
|---|---|---|---|---|
| uvloop/loop.c (Cython) | 8.7 | 0.556 | 0.177 | 0.092 |
| lupa/lua54.c (Cython) | 2.2 | 0.132 | 0.061 | 0.045 |
| zstd/zstd.c | 2.2 | 0.080 | 0.071 | 0.030 |

Language server on `uvloop/loop.c`: open to diagnostics 0.35 s, a line
typed mid-file 0.13-0.17 s. Cython's error-position macro expands
`__LINE__` on almost every error path, so cells below an edit depend on
absolute lines and rerun (19 of 80 here, 0.02 s in parallel); making
`__LINE__` results relative to their cell would keep them.

Found by the corpus: `__has_attribute` answered only for names the host
headers ask about, so zstd's `no_sanitize` read as unsupported.
tools/probe-host.sh now probes every identifier-like name in the
compiler proper (about 170k candidates, one `-E` pass, ~14 s once per
build directory).
