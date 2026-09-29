# C parser: plan (draft)

Goal: a C99 + GNU parser and type checker whose results survive edits at
sub-declaration granularity, so huge generated files (Cython output,
tables, macro-heavy code) re-parse in time proportional to the edit.

## Decisions so far
- Hand-written recursive descent, Pratt for expressions, typedef feedback.
- Flat post-order node arrays with unit-relative positions (Zig/Carbon).
- Incrementality built in from the start: **option 3**, content-defined
  runs of statements and initializer elements (our cells, applied to
  syntax), below per-top-level-declaration units.
- C99 + the GNU extensions real code uses; C11 keywords parse-only.

## Options considered (sub-declaration incrementality)
| # | Option | Pros | Cons |
|---|---|---|---|
| 1 | Top-level declarations only | simple; clangd/Roslyn-like early cutoff | a 1M-line generated function or table is one unit |
| 2 | Middle ground: top-level units, statement runs added later | smaller first step | retrofitting entry state and relative storage touches every node kind |
| **3** | **Content-defined statement and initializer runs** | edit cost bounded inside huge functions and tables; same machinery as preprocessor cells | entry state must be exact; most machinery up front |
| 4 | Lazy body parsing (skip bodies by brace matching, parse on demand) | cheapest open | not incremental by itself; deferred, compatible with 3 (P5) |
| 5 | tree-sitter-style GLR with edit ranges | proven for editors | no typedef feedback; not a semantic front end |

## Input: tokens (P0)
Measured token counts after preprocessing: `macro_heavy.c` (35 MB) 17.2M,
`table.c` (69 MB) 20.4M, uvloop `loop.c` (8.7 MB) 1.6M.  At 16-20 bytes a
token, storing them in the cells would cost 275-410 MB on the big inputs,
on top of the language server's 830 MB peak.  Options considered:

| # | Option | Memory (35 MB file) | Notes |
|---|---|---|---|
| 1 | Compact tokens per cell (columns, varint locations) | ~100-140 MB | simplest parser input |
| **2** | **Regenerate: re-run the preprocessor over the cells the parser needs; keep a count and hash per cell** | **~0** | chosen |
| 3 | Hybrid: kind + spelling id per token (4-5 B), locations regenerated | ~80 MB | option 2 plus a token cache; can be added later |

As built (`src/toks.[ch]`):
- Every cell records how many tokens it gives and their sequence hash
  (`Cell.ntoks`, `Cell.tok_hash`).  Workers hash each token as they go
  and keep (count, hash) marks at clean boundaries; a cell's record is the
  difference of two marks (polynomial hash mod 2^61 - 1, `hash.h`), so
  any run of cells has a hash computed from the cells' own.
- Token hashes cover kind, punctuator, spelling and provenance class
  (`TF_ORIGIN_*`, `TF_PASTED`, `TF_SYNTH`), not locations or white space.
- `ParOptions.toks` receives the build's plan and its cells in order
  (`TokRegen`); a `TokCursor` re-runs any run of whole cells against the
  build and yields its tokens with their presentation locations.  Cursors
  are independent (any number, any threads) and give their scratch space
  back when closed.  A cell may end right before a directive, so the
  preprocessor gained a stop at any plan item (`PP.plan_stop`), which also
  reports whether the stop was clean.
- Checked by `index --replay --tokens[=check|digest]`: the stream
  regenerated from the cells, each cell's record verified, equals a
  sequential run (tests/fuzz_cells.py, bench/corpus.py).
- Measured (`macro_heavy.c`, 17.2M tokens, 437 cells, 4 cores): no
  memory beyond the records and the kept plan (2307 items); phase B about
  3% slower for hashing (identifiers hash through a 64-bit digest cached
  in `Ident`); regenerating the whole stream sequentially 1.2 s, about
  3 ms per cell.
- Found on the way: a line starting with `##` (`%:%:`, a spliced `##`) was
  taken for a directive by the skip scanner, in phase A and in skipped
  groups (tests/pp/hashhash_line.c).

Next for the parser input: read tokens a cell at a time (not one stitched
array); on a cold open, consider capturing the fresh cells' tokens during
phase B instead of regenerating them.  Reuse checks: a parser unit whose
cells were all reused, in the same order, has identical tokens.
**`__LINE__`** (Cython): cells below an edit recompute and differ only in
line-number literals; parser keys should treat number spellings as holes
and give their values to constant evaluation only (P3).

## Tree
- Per unit: `Node{tag:u8, flags:u8, tok:u32 (relative), extra:u32}` in
  post-order, plus `extra[]` for variable-length data.
- Macro provenance is free: node → token → location → expansion.
  Diagnostics store unit-relative token positions and render the macro
  trail when emitted, so they never go stale.
- Types are hash-consed per build; a unit stores builtin ids or indexes
  into its read set (like `rmacro` in cells).

## Parser
- Scopes are **versioned per identifier**, like MacroTab: a history of
  (seq, entry) per slot; a snapshot is a sequence number.
- Jourdan–Pottier scope rules as tests: declarator scope before the
  initializer, parameter scope reopened for the body, `for` scopes, enum
  constants shadowing typedefs, `T(T)` ambiguities.
- **Error recovery**, synchronising at `;` and `}` of the current level,
  plus **indentation-aware brace recovery**: while brace depth > 0, a line
  at column 0 that can start an external declaration closes the open
  blocks (diagnostic at the unclosed `{`).  Without it, typing an
  unclosed `{` re-parses the rest of the file as statements on every
  keystroke.

## Units
| Level | Cut | Entry state (key) | Exports |
|---|---|---|---|
| Top-level declaration | its `;` or `}` | reads only | declarations, **summary** fingerprint (type, kind, linkage) |
| Statement run | after a statement whose own token hash hits the mask; min/max run length; recursive in big nested blocks | reads; return type; `break`/`continue` allowed; in `switch` | block-scope declarations; `case` values; labels defined and used |
| Initializer run | between top-level elements, same rule | current object type; designator cursor **below the top-level index** (the top-level index is relative) | elements with relative indices; cursor at exit |

- Checks across runs happen at the join: duplicate `case` values, `goto`
  targets, redeclaration conflicts, array size from the highest index.
- Reads: every identifier looked up, the kind found (typedef, ordinary,
  tag, none), and that entity's summary fingerprint.  Misses count.
- Min/max run lengths break content-defined cutting locally (boundaries
  shift until the next natural cut); keep max large.
- Initializer brace elision makes the cursor a subobject path; only the
  top-level index is relative.  Needs its own tests.

## Reuse: change-driven invalidation
Validating every read every build is O(file): the same walk that costs
~0.2 s per edit in the preprocessor.  Instead (Salsa's red-green):
1. Units whose tokens or entry state changed re-parse.
2. Diff their exports against the previous build: the names whose binding
   changed (added, removed, summary changed).
3. An inverted index name → reading units gives exactly the units to
   invalidate; repeat until no summary changes (early cutoff).
4. Everything else is reused without being looked at.

Candidate lookup, where needed: buckets by (first statement hash, entry
state hash), trying the unit at the old mapped position first, since
Cython repeats statements like `__Pyx_XDECREF(x);` thousands of times.

Reused units are immutable and reference counted (like `Cell`) and
spliced by reference.  Units with errors are cacheable.

## Type checking
Per unit, under the same key.  The C99 constraints of 6.5–6.9: operand
types, conversions, lvalues, `sizeof` and constant expressions (array
sizes, `case`), `const` violations, call arity.

## GNU extensions
Statement expressions, `typeof`, `__attribute__`, `__asm__`,
`__extension__`, `__label__`, `&&label` / `goto *p`, `case a ... b`,
`[a ... b] =`, `?:` with omitted middle, `__builtin_offsetof`,
`__builtin_va_arg`, `__builtin_types_compatible_p`, `__int128`,
`_Complex`, zero-length arrays.  C11 parse-only: `_Static_assert`,
`_Noreturn`, `_Alignas`, `_Generic`, `_Atomic`.

## Testing
- **Restart check by induction** (`--check-parse-restart`): from each
  boundary's snapshot, parse only the next unit on a fresh parser and
  compare its output and exit state with the straight run.  O(n).
- `tests/fuzz_parse.py`: random C, random edits (including typing an
  unclosed `{`), cached vs scratch (trees, diagnostics, types), directed
  cases with expected reuse counts.
- Jourdan–Pottier suite.
- Corpus: accept/reject parity with `gcc -fsyntax-only` on all units, and
  on **mutated** units (a token deleted or duplicated), comparing only the
  verdict; mid-file edit re-parse equals scratch.
- ASan/UBSan throughout; TSan from P5.

## Milestones
0. **P0** (done) per-cell token counts and hashes, token regeneration.
1. **P1** tree format, full parser without incrementality, brace recovery,
   `cereal parse --dump`, `-fsyntax-only`; Jourdan–Pottier; gcc parity.
2. **P2** scopes, types, summaries, type-checking diagnostics.
3. **P3** top-level units, change-driven invalidation, early cutoff,
   `fuzz_parse`, restart check.  Decide tree retention (below).
4. **P4** statement and initializer runs; directed tests on generated
   switches, tables, deep nesting.
5. **P5** parallel body parsing and checking (a body needs only the file
   scope at its start).  Join-time checks: block-scope `extern`, implicit
   function declarations, tags declared in prototypes.
6. **P6** LSP: definition and references for C symbols, hover types,
   document symbols, semantic tokens (typedef/variable/function),
   diagnostics; index tables per unit.

Targets: 35 MB file, edit → re-parse ≤ 50 ms; uvloop `loop.c` full parse
within 2x of `cereal -E` (0.18 s sequential).

## Open questions
- Tree retention: all trees (~60 MB+ on the 35 MB file) or summaries and
  index tables for cold units with re-parse on demand.  Measure in P3.
- Run parameters: mask 1/32 statements, min 8, max 256 to start.
