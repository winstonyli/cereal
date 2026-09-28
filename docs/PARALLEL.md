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
- a **persistent macro table**: `Ident → history` already records
  `def_seq`/`undef_seq`, so the live definition at version *S* is a lookup,
  not a copy;
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

## Recommended order
1. **(a)** TU pool in the driver (`-j`), for builds and multi-file lint.
2. **(e) + graph dependency sets**: phase A/B with split synchronization,
   verified against sequential mode by a differential test.
3. **(d)** header memoization on disk, keyed by graph closures.
4. **(f)** templates, only where profiling on real generated code shows
   repeated expansion dominating.

(b) and (c) are dropped, because (e) subsumes them.
