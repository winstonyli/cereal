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
