# Parallel preprocessing: survey and design (draft)

## Why phase 4 looks sequential
1. **Macro state changes over time.** Every point in a TU sees the table
   left by the `#define`s and `#undef`s before it.
2. **Conditionals depend on that state**, and `#include` splices in a file
   whose meaning depends on it too.
3. **Invocation boundaries are not lexical.** `F` on one line and `(` three
   lines later is one invocation, and rescanning can join an expansion's
   tail with the text that follows it (`f(1)(2)`).
4. **A few builtins depend on order:** `__COUNTER__`, and state-changing
   `_Pragma`s (`push_macro`, `poison`, `once`) reachable from text.

## Measurements (4-core container)
| step | macro_heavy 35 MB | table 69 MB | comments 57 MB |
|---|---|---|---|
| full `-E` (single thread) | 1.19 s | 0.61 s | 0.23 s |
| **directives-only scan** (skip scanner, SSE2) | **19 ms** | **34 ms** | **19 ms** |

Everything *except* directives is 97–98% of the work, and all of it is
text between directives.

## Options

| # | Option | Parallel unit | Ceiling | Cost |
|---|---|---|---|---|
| a | Many TUs per process on a thread pool | TU | cores | low: TU state is already independent |
| b | Pipeline lexer → pp → printer | stage | ~1.8x | medium; subsumed by (e) |
| c | Chunked lexing of one file (split at certified line starts) | chunk | lexing is ~1/3 of time, so ~1.3x | medium; subsumed by (e) |
| d | Per-header memoization: key = (header, values of the macros it reads) → (tokens, macro-table delta) | header × state | removes repeated work across TUs | the planned disk cache, made sound by the dependency sets below |
| **e** | **Two-phase state-split preprocessing** (below) | text segment | ~30x by Amdahl; ≈ cores in practice | high |
| f | Macro "templates" (partial evaluation of a macro's expansion with parameters as holes) | invocation | single-thread speedup of expansion; makes (e) segments cheaper | high; a correctness minefield, needs applicability conditions |

Prior art:
- clang-scan-deps "minimizes" sources down to their directives and then
  preprocesses only those, which is about 10x faster than full preprocessing
  for dependency discovery ([D63907](https://reviews.llvm.org/D63907),
  [slides](https://llvm.org/devmtg/2019-04/slides/TechTalk-Lorenz-clang-scan-deps_Fast_dependency_scanning_for_explicit_modules.pdf)).
  Phase A below is the same idea, used to *drive* full preprocessing
  instead of replacing it.
- Speculative and enumerative parallel DFA matching splits the input and
  resynchronizes chunk states
  ([Mytkowicz et al.](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/asplos302-mytkowicz.pdf),
  [Certified split points for parallel lexing](https://arxiv.org/abs/2608.03473)).
  Segment synchronization below is the same move, one level up: at the
  macro-expansion state rather than the lexer state.
- SuperC and TypeChef preserve every configuration through preprocessing
  ([SuperC](https://paulgazzillo.com/papers/pldi12.pdf),
  [TypeChef](https://github.com/ckaestne/TypeChef)). This is relevant to
  multi-configuration verification later, not to throughput.

## (e) Two-phase preprocessing

**Phase A: sequential, directives only.** The skip scanner walks the TU,
executing `#define`, `#undef`, `#if`, `#include`, `#line` and `#pragma`.
Only `#if`, `#include` and `#line` operands are macro-expanded. The result:
- **segments**: maximal runs of active text between directives, each tagged
  with the macro-table *version* at its start;
- a **persistent macro table** (`MacroTab`, per build, indexed by
  identifier id): each name's history records `def_seq`/`undef_seq`, so the
  live definition at version *S* is a lookup, not a copy. Poisoning is
  versioned the same way;
- block-comment state at every line start, so any line start becomes a
  certified split point.

**Phase B: parallel, text only.** Segments, with huge ones split at
certified line starts, go to a worker pool. Each worker lexes, expands
against version *S* and formats output (or emits listener events) into its
own buffer. Buffers are concatenated in order.

**Synchronization at splits.** A worker starts at split *s* assuming an
empty expansion stack. The previous worker runs past *s* until its first
top-level token boundary *p* with an empty stack and no pending `F`-`(`
lookahead. If the next worker was also clean at *p*, both are in the same
state from there on: take the earlier worker's output up to *p* and the
later worker's from *p*. Otherwise re-run from *p*.

In generated code nearly every line ends clean, so the overlap is about one
line. Invocations whose arguments contain directives, which GCC processes,
simply become one segment.

**Order-dependent pieces:**
- `__COUNTER__` becomes a placeholder, patched by a prefix sum over segments.
- A state-changing `_Pragma` found in text aborts parallel mode from that
  point, and processing continues sequentially.
- Diagnostics and listener events are buffered per segment and replayed in
  order, so the result is **byte-identical to sequential mode**. That is
  testable: sequential vs. parallel is itself a differential test.

## Where the macro graph comes in
The graph: nodes are macro *definitions* (versions); edges are identifiers
in a body that name macros, resolved per version. It is not a DAG, because
cycles are legal (`A→B→A`, self-reference). Condensed into strongly
connected components it *is* a DAG.

The graph does not split the work *by itself*, because the unit of work is
text, not macros. Expanding one invocation is an inherently sequential
rescan. What the graph gives is the **dependency sets** that make
state-based splitting sound, incremental and cacheable:

1. **Segment inputs.** A segment reads only the transitive graph closure
   of the identifiers in its tokens (plus any identifier formed by `##`,
   which is caught dynamically). If none of those identifiers changes
   between versions *S₁* and *S₂*, the segment's output is the same in both.
2. **Pipelining phase A and phase B.** A segment may start before phase A
   has finished the file, once no later directive before the segment can
   redefine anything in its closure.
3. **Incremental re-preprocessing (LSP).** Editing a `#define` invalidates
   only the segments whose closure contains it, not every line after it.
   That matters when editing huge generated files.
4. **Header memoization (d).** A header's input set is the closure of the
   identifiers it tests or expands; the cache key is those identifiers'
   definitions. This is what makes an automatic, sound "precompiled header"
   possible.
5. **Templates (f).** A macro's compiled template is valid while its closure
   is unchanged.

## Decisions (upfront cost is not a constraint: do it right)

| Choice | Decision |
|---|---|
| Options | (a) TU pool + (e) two-phase + the macro graph and dependency sets now; then (d); (f) only with profiling evidence (its risk is correctness, not effort) |
| Splits | directive boundaries and certified line starts; speculate-and-verify stitching |
| Phase B input | the *phase-A stream*: active text ranges in order, with **directive markers** wherever a directive was removed. A marker behaves like the BOL `#` it replaces: it ends a `(` lookahead and advances the macro version inside argument collection. This is exactly what the sequential engine does. |
| Macro state | Versioned lookups in the immutable history. "Currently expanding" moves off `Macro` into each worker's context stack. `pop_macro` creates a new version instead of reviving an old one. |
| Shared structures | Interner split into independently locked shards with a two-level id table; per-worker scratch space; the file registry published safely to readers; per-worker arenas, token pools and diagnostics |
| Listeners | Thread-aware: per-worker analyzer state with a deterministic, ordered merge. Order-sensitive consumers get events in sequential order. |
| Output | Byte-identical to sequential mode, always. Workers emit text plus boundary printer state, and the merge recomputes boundary transitions (newlines, linemarkers, spacing). |
| Unsafe constructs | `__COUNTER__` (values can feed `##`) or a state-changing `_Pragma` (`once`, `push_macro`/`pop_macro`, `poison`, `system_header`): sound detection, with sequential fallback from that point |
| Parallel or not | Adaptive, from phase-A statistics (text size, segment count, cores) |
| Verification | Sequential vs. parallel differential tests on every test input plus a fuzzer that generates adversarial split points (invocations spanning splits and directives) |

## As built (step 4)

`cereal -E` runs `par_write_output` (src/par.c) unless `-fparallel=off`.
`-fparallel=auto` (the default) goes parallel only when the main file is at
least 4 MB and more than one core is available, so small TUs never pay for
phase A. `-fparallel=on` forces the machinery (tests use it with tiny chunks).
`-fparallel-threads=N`, `-fparallel-chunk=BYTES` (segment split size, default
64 KB) and `-fparallel-window=N` (see below) tune it. `CEREAL_PAR_STATS=1`
prints phase timings, slices and per-worker spans; `=2` also dumps the plan.

- **Phase A** is the full engine in `PPM_PHASE_A`. Text is not lexed. It is
  recorded as `PI_SEG` items, split at line starts that the skip scanner
  certifies (never inside a comment). Directives become `PI_DIR` items;
  include entry and exit become `PI_ENTER` and `PI_EXIT`; pragmas destined
  for the output become `PI_PRAGMA`. Every item carries the macro version,
  the `__COUNTER__` value and an immutable `PlanFrame` (a new one after
  `#line`).
- **Partition**: ranges of segment items with equal text bytes, one per
  worker. Every range starts at a segment.
- **Workers** (`PPM_PLAN`) look up macros by version and print into memory.
  For the first `window` segments after its start (default 256), a worker
  publishes a `BoundRec`, lock-free: whether it was *clean* there (the
  top-level read, no context, no carried space), and the first token it
  printed after that point.
- **Stitching**: past its end, a worker stops at the first boundary where it
  and the owner of that item were both clean, and where the owner's first
  token after the boundary has a source position. After a positioned token,
  the printer state is a function of that token alone. Two edge cases were
  made to hold this: re-entering the same file, and a backward line with no
  linemarker. The merge re-renders just that one transition from the
  predecessor's state, carrying file enter and exit flags across, then
  copies the owner's bytes. If no boundary qualifies (an invocation that
  spans the whole window, or the owner is not there yet), the predecessor
  runs on. Stitching is never required for correctness.
- **Diagnostics** carry the plan item they were reported at. Workers keep
  those of their slice. The merge sorts by (item, worker before phase A),
  stably. A worker diagnostic at a DIR or EXIT item comes from an invocation
  reading into that directive or the file end, which sequential mode reports
  first. Phase A does not report lexer diagnostics in segment text; the
  workers do.
- **Divergence**: `__COUNTER__` and the state-changing `_Pragma`s abort
  parallel mode for the whole TU, and it is rerun sequentially. The prefix
  sum and resuming from the divergence point are future work.

Verification: `tests/run.sh` checks that output, diagnostics and exit status
are byte-identical to sequential mode. It covers every test input, the
dogfood sources and some system headers, at thread/chunk/window settings
down to 1-byte chunks and 1- or 2-segment windows. `tests/fuzz_par.py`
generates programs with invocations that span splits, directives and
include ends, `#line`, `_Pragma`, splices and multi-line comments. Both
also pass under ThreadSanitizer.

Found on the way (sequential bugs too): phase A and `skip_group` re-seeked
to the pending token's line start by scanning backwards, which can land
inside a multi-line comment (`#if 0` followed by `/* ... #endif */`). The
lexer now records the last real line start it crossed. `#line` with a
backward line number printed no linemarker. The null-character warning was
issued once per file instead of once per line (GCC's behavior).

## Recommended order
1. **Done.** Concurrency foundation: threads, pool, atomics, concurrent interner,
   thread-safe source manager.
2. **Done.** Preprocessor split into shared and per-worker state; versioned macro
   lookup; per-worker disabled macros.
3. **Done.** Phase A (directives only), producing the phase-A stream.
4. Phase B workers, stitching, ordered merge, parallel `-E`, differential
   tests and a fuzzer. **Done** (see "As built").
5. **Done.** Thread-aware listeners: `lint` and `index` (and `query`) run
   two-phase like `-E`. `par_run` takes *clients* (src/parclient.h):
   - phase A's events reach the main listeners live;
   - `fork` gives each worker private listener state;
   - `prepare` runs per slice concurrently, filtering to the slice;
   - `join` merges in slice order;
   - `finish` completes.
   Details:
   - Expansion counts come from a per-worker log, joined for kept slices
     only; workers never touch the shared counter.
   - Hygiene's invocation checks report through the worker's engine.
     Their "once per location" rule becomes a `once` tag, resolved by the
     ordered diagnostic merge.
   - The include analysis joins "macro used" marks.
   - The index keeps worker records in place (the TU adopts worker arenas),
     renumbers every expansion into sequential order concurrently, and
     merges per-worker sorted refs.
   Refs now have a total order and an exact global "one per (location,
   macro)" dedupe. `_Pragma` diagnostics are located at the `_Pragma`, not
   in scratch space. Output is byte-identical to sequential for all three
   modes (tests/run.sh, fuzz_par.py). macro_heavy: lint 1.32s -> 0.74s;
   index build 2.30s -> 1.23s. The JSON writer is now buffered: stdio
   locks per call once threads exist, which made `index` output 2x
   slower. Still sequential: the ref merge and JSON output.
6. **Done.** TU pool (`-j`). All inputs of `-E` and `lint` run as jobs on
   one shared pool, which also runs the phase-B workers. The waiting thread
   helps, so the nesting cannot deadlock. Each job buffers its output and
   diagnostics, and the main thread writes them in input order as they
   finish. Process-wide state was removed first: `-W` configuration is an
   immutable `DiagConfig`, the predefines buffer lives in `PP`,
   `__DATE__`/`__TIME__` are fixed once per process (this also fixed a
   use-after-free across TUs), and one-time tables use `pthread_once`.
   Tested: `-j1` and `-j8` are byte-identical (tests/run.sh), and clean
   under TSan. 20 dogfood TUs: 0.097s at -j1, 0.034s at -j4.
7. **Done.** Macro graph and dependency sets (`src/mgraph.[ch]`, see
   ARCHITECTURE.md "Macro graph"). Closures per version, with names that
   are not macros included and definitions that can form names by `##`
   marked open. Consumers so far: call hierarchy, `deps`,
   `-Wmacro-recursion`. Soundness is checked against real expansions on
   every test input and fuzzed programs. Using the closures to pipeline
   phases A and B is left for when phase A is the bottleneck (`skipped`).
8. (d) Header memoization on disk, keyed by the dependency sets.
9. **Done.** Cell cache: phase B reused across builds of an edited unit
   (the language server). See "Cells" below and docs/LSP.md.
10. **Done.** Index kept per cell, queries walking cells (see "Cells").

(b) and (c) are dropped, because (e) subsumes them.

## Cells

A *cell* is a run of plan items that a worker preprocessed from a fresh
start and left clean, so its results depend only on what it read
(`src/cell.[ch]`). With a cache (`ParOptions.cells`, no `-E` output):

- Phase A splits text at **content-defined** points (a line whose hash
  says so, about every `chunk` bytes, at least `chunk / 4` apart; the
  first segment after a directive when due). An edit moves only the
  boundaries next to it. Items that begin at such a point are the
  candidates where cells start.
- Before phase B, a lookup at every candidate (and at the end of every
  reused cell) finds cached cells whose key still holds. Workers cover
  the rest; a worker that reaches a reused cell's start cleanly stops
  there, one that reaches it inside an invocation runs on.
- After phase B, every slice a worker ran is split at the clean candidate
  boundaries it crossed and stored as cells.
- Diagnostics and analyses: reused cells are decoded into workers that
  never run, and all slices join exactly as in a normal run (cheap).
- The index takes the cells as they are (`ParClient.place`; the language
  server, and `index --replay --transcript`): see "Index in cells" below.
  Only when every slice has a cell; otherwise it decodes and joins too.
  `index` JSON and `--check-graph` always use a materialized index.

**Relative storage.** A cell keeps nothing of the build that made it,
except identifiers (a unit's builds share an interner):
- locations as (item of the cell, offset), (definition it read, offset
  from its `#`), or (ancestor frame of an item: an `#include` line
  outside the cell);
- definitions as indexes into its read set (`pop_macro` copies through
  their original);
- versions as the item that set them (`Expansion.seq_item`).
A location that fits none of these makes the cell uncacheable.

**Key.** Checked on reuse:
- items: kind, split and cut flags, segment text, pragma text, frame
  (file, presumed name, depth, system flag), all hashed;
- the read set, recorded by workers: every (identifier, item) looked up
  or lexed, with the fingerprint of what was found (a definition's text,
  file and flags; none; poisoned or not). Reads that found one definition
  must find one definition again, and different ones different ones
  (an identical redefinition can appear between two reads);
- presumed line numbers, if `__LINE__` was expanded;
- whether the cell ended the TU;
- options (once per cache).
A cancelled or halted build stores nothing and evicts nothing. Cells are
reference counted: a snapshot's index keeps its cells after the cache
drops them.

**Index in cells.** As in Roslyn's red-green trees, the cells are the
green part (relative, shared by builds) and the build supplies the red
part: the plan's item start positions, and what each cell's reads found.
clangd-style layers: phase A's records (definitions, `#if` expansions,
includes, conditionals) are rebuilt each build; the text's stay in the
cells, which carry, made once at encode time:
- refs sorted by relative location, and a name -> refs index;
- a summary of observed calls: (parent definition, child definition,
  name origin) with counts and first occurrence.
Queries gather what they need and put it in the materialized order:
- covering a location: the items whose text meets the window
  [loc - longest ref, loc] (binary search over item ranges), and the
  definitions containing it (through a table of which cells read them);
- by name (references, rename): each cell's name index, as rust-analyzer
  narrows find-usages before resolving;
- in a file (semantic tokens): every cell's refs;
- call hierarchy: the call summaries.
Expansions have no global ids: an `order` (plan item, a worker's before
phase A's, local id) sorts and deduplicates exactly as the ids would.
Hover builds the one expansion it shows.

**Tests.** `tests/fuzz_cells.py` follows random programs through random
edit sequences (lines inserted, deleted, replaced, moved, characters
changed, in the main file and headers) with pairs of `cereal index
--replay` processes, one reusing cells and one sequential without a
cache. Every build must be byte-identical within a pair: diagnostics,
the materialized index (JSON), and a query transcript (`--transcript`:
every identifier of every user file resolved, its references and call
hierarchy, every file's refs), which the cached side answers by walking
cells. Directed cases cover the checks random edits rarely reach
(removing any of them fails the suite): an identical redefinition
between two reads, a moved definition, `__LINE__` below an edit,
poisoning added above, a header changed under an unchanged main file.
They also assert reuse, Sorbet-style: a moved definition must reuse a
cell, an unchanged rebuild every cell, the others must recompute one.
The LSP scenarios also run with the parallel path and cells forced
(`.cereal`: `-fparallel=on -fparallel-chunk=1`) against the same
transcripts. All of it also under TSan and ASan/UBSan.

Found on the way (bugs in parallel runs since step 4): a segment's lexer
read past the segment when the text after it began with an unterminated
comment (a crash), and a comment running to the end of a file was never
lexed by a worker, so "unterminated comment" went unreported. The
generator now produces such files.

**Measured** (35 MB macro_heavy.c, 4 cores, `index --replay`):

| Build | Materialized index | Index in cells |
|---|---|---|
| first (all cells computed and stored) | 2.1 s (1.6 s without a cache) | 2.1 s |
| a line inserted mid-file (1 cell rerun) | 0.3-0.5 s | 0.11 s |

Of an edit in cells: phase A 0.06 s, lookups 0.02 s, the rerun cell
0.015 s, the rest joins of diagnostics and analyses. Memory (3 builds):
900 MB without a cache, 1.0 GB with cells, 1.3 GB materialized.
