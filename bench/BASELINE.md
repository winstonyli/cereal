# Baseline (before token-core rewrite)

Commit d18f453-era core (eager linked-list tokens, Prosser hide sets). Best of
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
