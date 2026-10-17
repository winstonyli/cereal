# S1 implementation plan (draft 5; Slice 0 and S1a done)

Scope: stage S1 of `docs/CONFIG_DESIGN.md` (section 8): BDD solver, presence
conditions and queries for the open unit, family table, hover and
`cereal/configExplain`, `cond.c` rewritten on top. The spikes (section 13) are
done; their numbers are inputs, and their Python figures are **not** gates for
the C code (node counts depend on the manager and the order).

Review log: round 1 (Sonnet) found 5 false or unverified tree claims, 6 design
contradictions, 4 ordering gaps and 2 untestable gates; round 2 found 4 more
(skeleton must stay entry-independent, arithmetic lost by the BDD, rule (a)
already exists, harness precedent); round 3 found 4 small ones (include.c consumers of skeleton kinds, no lint
timing tool, corpus gates, truncation cases only guard against crashes); this
draft folds all three in. Stopped after round 3 by judgment: the remaining risk is
in execution, which the slice gates exercise.

## Facts about the tree (verified)

- `src/analysis/cond.c` (925 lines) checks each user file's `#if` chain by
  enumeration over at most 16 variables (`Problem.vars[16]`), 63 conjuncts
  (`:543-552`, silently dropped beyond that), `Frame stack[64]` (`:739`, silently
  returns when full), 2^18 assignments, `dom[24]`. It runs in the sequential
  directive-only phase A (`analysis.c:79`: "cond: #if/#ifdef events only"), and
  `an_fork` copies the pointer, so cond state is shared and output must stay
  byte-identical under `-fparallel=on` (`tests/run.sh` `par_same`).
- The lint tests are `// expect:` annotations in `tests/lint/cond.c`, checked by
  `tests/verify.py`; they do not exercise the limits above.
- `Skeleton` gives each directive's kind, tokens, depth and opener. It maps
  `include_next` to `SK_INCLUDE`, `#pragma` to `SK_OTHER`, has no
  `#elifdef/#elifndef` kind and no resolved include path.
- `cond.c` `eval` decides `* / % + - << >> & ^ |`, `~`, the ternary and
  variable-free conditions (`#if 1+1==2`) over its small domain; unary minus and
  character constants are already parsed. `skel.c` treats `#elifdef/#elifndef`
  as `SK_OTHER`, so lint today ignores them as chain boundaries (an existing
  bug that S1b fixes).
- There is no C unit-test harness: `Makefile` has `test`, `bench`,
  `bench-quick`, and `SRCS` wildcards `src/analysis/*.c`.
- The S0 corpora are not in the tree (`bench/fetch_corpus.sh` fetches PyPI
  sdists; the S0 libuv set was 62 units).

## Slice 0 (before any code): test scaffolding

**Status: done (Round 249).** Delivered: item 2 as `tests/lint/cond_limits.c` (current
expectations; the gap cases are the reviewed diffs of S1a) and item 4 as
`bench/bench.py --lint` with the baseline in `bench/BASELINE.md` (100.7 ms
total). Items 1 and 3 move: the `--selftest` hook lands with the BDD test in S1a,
the fold fixtures with S1c (nothing exists to test earlier).

1. **Harness:** a hidden `cereal --selftest <name>` subcommand (`main.c` already dispatches
   `index-summary` on `argv[1]`), with `tests/run.sh` calling it; the BDD test
   (ite/and/or/not against brute force on 8 variables, node-cap behavior) is the
   first user.
2. **Lint cases for every cond.c limit** (more than 16 variables, 2^18, `dom[24]`,
   64 conjuncts, 32 priors, 64 frames, unmodeled condition, `#elifdef`) added to
   `tests/lint/` with their **current** expectations. The truncation cases (silent
   today, `unknown` with no diagnostic after S1a) only guard against crashes and
   wrong answers; the cases whose answers S1a changes (arithmetic, variables
   beyond 16, `#elifdef`) are the reviewed diffs.
3. **Fixtures:** small in-repo fixtures that exercise the fold (guards, `pragma
   once`, thresholds, families, system-header macros, a computed include); the
   libuv, Lua and zstd corpora stay a documented, optional gate (`bench/` fetch
   plus unit lists recorded in `docs/`).
4. **Timing gate:** a new `bench/bench.py --lint FILES` mode (it times only `-E`
   today) on in-repo fixtures plus the Lua sources from `corpus.py` (best of 5, 10%
   noise margin), baseline recorded in `bench/BASELINE.md` **before S1a lands**,
   with the CPU load and Defender state (LONG_RUNS.md).

## Slices (each leaves the tree green; commit per slice)

**S1a, BDD manager and `cond.c` on top (lint semantics unchanged).**
`src/analysis/bdd.[ch]`: arena nodes, unique table, `ite` with memo, `and`,
`or`, `not`, `support`, `eval`, node cap failing with `BDD_OVER`. Rewrite
`satisfiable()`/`build()` as BDD conjunction over the frame stack; delete
`Problem`, enumeration and the domain machinery in the same commit. Threshold
atoms `GE(X,c)` with monotonic implication clauses applied per group (decision
1). Lint mode keeps its contract (section 4: defines not followed, no
predefines): of the Round 247 rules only (c) applies (rule (a) already exists in the
parser); (b) alias chase needs macro bodies and moves to S1c. Rule (c), a
comparison of two values (VAL vs VAL), becomes an OPAQUE atom keyed by canonical
text in S1a (the case split over a common threshold set is S1c's evaluator work,
with the alias chase).
Arithmetic on atoms (`X & 4`, `X % 2`, `A + B`) becomes an OPAQUE atom per
directive (section 3.2), a **known loss** of dead/redundant detection the old
enumeration had; variable-free subtrees keep a constant folder (`#if 1+1==2`,
ternary, `~`). Slice 0 adds lint cases for arithmetic, ternary and constant
folding; `Frame stack[64]` becomes a growable vector. An unmodeled condition stays
`unknown` with no diagnostic. The silent truncations (63 conjuncts, 64 frames)
become `BDD_OVER`/`unknown`. Gate: `make test` green; the expectations changed
in Slice 0 item 2, including the arithmetic known diff, are the only diffs and each is reviewed; lint timing within
the margin; byte-identical output under `-fparallel=on`.

**S1b, skeleton kinds and include resolution (prerequisite for the fold).**
*Status: skeleton kinds done (Round 251: `SK_ELIFDEF`, `SK_ELIFNDEF`, `SK_PRAGMA_ONCE`, `SkDirective.next`; `cond.c` and `include.c` updated, `cond_limits.c` expectations reviewed). The per-unit resolved include edges and their `--selftest` are **parked to S1c**: nothing consumes them before the fold, and their shape is best fixed by it.*

`Skeleton` stays **entry-independent** (cached per file, re-derived from content,
section 5): `skel.h` gains kinds for `#elifdef/#elifndef` and `#pragma once`, and
`include_next` becomes a **flag on `SK_INCLUDE`** (not a kind), because
`src/analysis/include.c:350` tests `kind == SK_INCLUDE` and `:275` lists the chain
kinds (`SK_IF/IFDEF/IFNDEF/ELIF`), which gain the two new elif kinds; `cond.c` treats `#elifdef/#elifndef` as chain branches.
Resolved include edges go in a separate per-unit structure keyed by (file,
entry), filled by a resolver using the entry's search paths. Gate: `include.c` behavior and its tests unchanged;
a reviewed `// expect:` diff for the `#elifdef` lint change; `tests/ppstd/elifdef.c` chains
modeled; resolved edges for the fixtures match the preprocessor's include events
(compared by a `--selftest` that dumps both).

**S1c, symbolic fold, host valuation and the oracle.** `src/analysis/pc.[ch]`:
the fold over the include closure (quoted includes symbolic, system includes
host-concrete, section 3.4), producing Local PCs per directive, exactness flags
and the assumption set A (Round 246). The valuation of the **initial** atoms
`I(X)` is the state before line 1: predefines plus `-D/-U` plus `-include` and
`-imacros` files (not the end-of-unit macro table, which would call a defined
guard a "defined" initial atom). Two-pass variable order (first pass discovers,
second folds with block-contiguous order). Opaque atoms per directive;
alias chase (b) lives here. Oracle (section 8): concrete decisions of the
preprocessor against the PCs under the valuation, at least 98% of decidable
groups, with coverage and the excluded share reported, plus solver-guided
overlay draws re-preprocessed with `-D`. Gates (section 8 bounds, not the
prototype figures): under 20,000 nodes per group; exactness at least 90% with A
listed; fold per entry under 1 s on average with p95 and max reported; the fixtures
pass. The numeric gates are stated against libuv, Lua and zstd, so they run
only with the corpora fetched (`bench/fetch_corpus.sh`, network) and their unit
lists and results are recorded in `docs/`; a slice commit may land on the
fixtures alone, with the corpus gate marked pending in the status line.

*Status: done (Round 253), except the exactness gate. Fold, host valuation, trace-following
system headers, oracle and the `CEREAL_PCDUMP` hook (`bench/pc_oracle.py` aggregates) are in.
Oracle: 100% agreement on exact groups on every corpus; nodes, time and cap gates met;
exactness 89-100% after the guarded-case value layer (Round 256), only zstd (89) under 90%; what
remains is feature tests and compiler arithmetic, which S1d's family values make exact. **Parked:**
the solver-guided overlay draws (need S1d's solver). The trace needs `-fparallel=off`, so the
fold is not yet reachable outside the hook (S1d/S1e wire it).*

**S1d, queries and the family table.** *(Status: S1d-1 done, Round 257: valuation, `sat`/`holds`, `preconditions`, `diff`, brute-force selftest, `why` hook. Remaining: family table, EMPTY/HAS, phases 2a/2b, `via`, rank-1 gate on fixtures.)* `sat`, `why_off` (phases 1, 2a, 2b, typed
`Requirement{clause,kind,status,source,via}` per 3.8), `diff`, ranks to K=3. The
family table with OS and compiler leaves, the overlay-flippable bit, cross-axis
clauses, the language block (std rows with derived `__STDC_VERSION__`/`linux`),
`EMPTY` value state (3.2) and `HAS` atoms; the settings-extensible part stays
**out of** `fingerprint` and manifests until S3. Gate: rank-1 cost against a
brute-force path enumerator kept in the test as the independent reference, on
the fixtures and on the optional corpora.

**S1e, surfaces.** Hover on `#if` directives, on `#define`s, and on inactive
declarations (needs the textual `inactive_use` scan and a group lookup, since
inactive text has no DECL events); `cereal/configExplain`. The fold runs per
open document revision and is cached by (file, content hash); a document served
from carry/cell caches without a live PP gets an `unknown` explanation until
reparsed. Gate: `tests/lsp/*.json` golden transcripts for the hover and the
request (this is the documented shape).

## Cross-cutting

- **No format change:** `CREC_FORMAT` stays 4; PCs live in memory until S3.
- **Delete, don't layer:** the enumeration code goes in S1a, not kept as fallback.
- **Parallel:** PC work runs in phase A or from the skeleton; no shared state
  is written from workers.
- **Docs per slice:** `docs/HISTORY.md` entry, `docs/ROADMAP.md` Track D status.

## Decided (were open questions in draft 1)

- `pragma once`, `#include_next`: synthetic guard, skipped include and the
  assumption set A (3.4, 3.6).
- New diagnostics stay off by default (section 4); a condition that is constant
  only because of host-concrete system macros is not reported by lint.
- Macro values for the fold come from re-lexing skeleton tokens as in the
  prototype (object-like numeric and alias bodies plus defined-ness), not the
  preprocessor's definitions; revisit only if the oracle shows divergence.
