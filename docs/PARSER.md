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
- Per unit (one external declaration): `Node{tag:u8, flags:u8, aux:u16,
  tok:u32, size:u32}`, 12 bytes, in post-order (`src/c/ast.h`).  `size`
  is the node's subtree size, so children are found by walking back from
  the parent (Carbon's layout); no child pointers, no `extra[]`.  `tok` is
  relative to the unit's first token.  Optional parts are told apart by
  tag class; `for` clauses use an `N_NONE` placeholder.
- Macro provenance is free: node → token → location → expansion.
  Diagnostics should store unit-relative token positions and render the
  macro trail when emitted, so they never go stale (P3).
- Types are hash-consed per build; a unit stores builtin ids or indexes
  into its read set (like `rmacro` in cells).

## Parser
- Recursive descent, precedence climbing for binary operators
  (`src/c/parse.c`); a unit at a time, streaming from any token source
  (the preprocessor directly, or cursors over a cell build).
- Typedef feedback from a scope table (`src/c/scope.[ch]`): per
  identifier, a chain of entries in an undo log; a scope is a mark in the
  log; a function declarator's parameter scope is saved when it closes
  and declared again for the body.  (P3 turns this into versioned
  lookups, like MacroTab, for snapshots.)
- Jourdan–Pottier scope rules (tests/parse/scopes.c): declarator scope
  before the initializer; parameter scope reopened for the body and ended
  with the parameter list otherwise; later parameters see earlier ones;
  `(T)` in a parameter declaration is a function declarator; `T T;`;
  enumeration constants shadow typedefs; `for`, selection and iteration
  statements and their bodies are blocks; member names shadow nothing;
  `unsigned T:3` vs `const T:3`; `T * b`; a label named like a type.
- **Error recovery**: one error per place (no cascades), synchronising at
  `;` and `}` of the current level.  **Brace recovery** as built: a
  function definition that starts in column 0 inside a function body ends
  the open body there (error at it, note at the unclosed `{`), and parses
  as a definition of its own.  C has no function definitions in blocks,
  and GNU nested functions are indented in practice, so valid code never
  triggers it; typing an unclosed `{` no longer turns the rest of the
  file into statements.
- Leniency follows GCC's: what it only warns about is accepted (implicit
  int, K&R definitions, a missing `;` before a struct's `}`, empty
  initializers and structs, labels at the end of a block, stray `;`).

### P1 as built: results
- **gcc parity** (`bench/corpus.py`): all 170 corpus units (Lua 5.1-5.5,
  libuv, two Cython outputs, zstd) are accepted, as gcc accepts them.
- **Mutations** (`bench/parse_mutate.py`): a token deleted or duplicated
  in preprocessed corpus code, 16 per unit per seed.  Across three seeds
  (about 7,400 mutants, one run under ASan/UBSan) cereal never rejects
  what gcc accepts and never crashes; about 10% of mutants are semantic
  errors only gcc reports (types, undeclared names: P2).  Found and fixed
  on the way: a missing `;` before a struct's `}` (a GCC warning), and
  `f(x) __attribute__((...));` taken for a K&R definition.
- **Cells**: parsing tokens regenerated from a cell build gives the same
  trees and diagnostics (corpus, dogfood, goldens at one-line cells).  A
  unit spans cells, so a cursor feeding the parser keeps its scratch
  spellings when it closes (`tokcur_close(c, true)`); regression test
  tests/parse/macros.c.
- **Speed** (`macro_heavy.c`, 17.2M tokens, 400k units): preprocess and
  parse 2.46 s against 1.41 s for `-E` alone, so parsing is about 60 ns a
  token; the corpus parses in 2.4 s where `gcc -fsyntax-only` takes 6.3 s
  (gcc also type-checks).
- **Memory**: a unit's tokens (20 bytes each) and nodes live until the
  next unit; `table.c`, one 20M-token initializer, peaks at 544 MB.
  Initializer runs (P4) bound this.

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

## Type checking (P2)
### Decisions
1. **Placement: a separate check pass per unit** (Carbon-style), run right
   after the unit parses.  An RPN stack machine over the post-order tree,
   one handler per node tag; the parser keeps only typedef feedback.  Side
   arrays per node: type id, resolved declaration, constant value.
   Bracketing marker nodes where post-order is too late: declarator end
   (scope starts there: `int x = sizeof x;`), compound-statement start,
   function-body start.  Function signature summaries are split from
   bodies (rust-analyzer).  Diagnostics come out in source order.
2. **Depth: all C99 constraints**, three severities: Error, Pedwarn,
   Warning/Silent.  Reject iff an Error.  Each check is tagged with gcc
   13's call kind so a later `-pedantic-errors` mode is a table flip.
3. **Target struct** (arocc-style): size/align per C type, separate
   struct-member alignment (i386), `max_align`, `char_signed`,
   `long_double_format` (x87/ieee64/ieee128), `size_t`/`ptrdiff_t`/
   `wchar_t`/`intmax_t` mappings, `va_list` shape, `layout_kind`
   (sysv/ms), the four clang bit-field flags
   (`ignoreNonZeroSizedBitfieldTypeAlignment`,
   `ignoreZeroSizedBitfieldTypeAlignment`,
   `minZeroWidthBitfieldAlignment`, `unnamedFieldAffectsAlignment`),
   `default_aligned_attr` (16 on x86-64).  Record layout ported from
   arocc `record_layout.zig` (itself from repr-c).
4. **Per-node types retained** (4 bytes/node); measured in P2f.
5. **Oracle: gcc 13.3 verdicts**, not dg annotations (which assume
   `-pedantic-errors`).  `gcc.dg/*.c` (6217 files) recorded under
   `gcc -std=c99 -fsyntax-only`; plus c-testsuite, cproc, tcc (accept
   side) and clang `test/Sema` C files (reject side).

### Types
Derived types hash-consed; qualifiers in the low bits of the u32 id.  A
tag type's identity is (declaring unit key, tag name or anonymous
ordinal); layout is a separate query, so completing or re-laying out a
struct never re-creates the type (unlike Zig).  Each type carries a
64-bit structural digest for summaries.

### gcc 13 severities (`-std=c99` and `gnu99` agree)
- **Warning, accepted:** implicit function declaration, implicit int,
  int<->pointer and incompatible pointer conversions, `return` value
  mismatches with void, untyped K&R parameter, excess initializers,
  string too long for its array, discarded `const`, pointer/integer and
  distinct-pointer comparison, `INT_MAX+1`, `static` in non-static
  inline, non-constant file-scope array size that gcc folds.
- **Silent:** empty TU, `int a[0]`, `struct s {}`, `;;`, function pointer
  to `void*`, `void*` arithmetic, `sizeof(void)`, label at block end,
  `int f(void){}`.
- **Error:** negative/non-integer array size, non-constant initializer
  (`1<<-1`, `1/0`), non-integer enumerator, file-scope VLA, undefined
  label, duplicate or non-constant case, assignment to read-only, bit-field
  too wide, redefinition/conflicting types, `long long long`,
  incompatible struct return.
- gcc 13 has no `permerror` in `gcc/c/`; `-fsyntax-only` only skips
  middle-end diagnostics (`__attribute__((error))`, asm constraints,
  always_inline, flow warnings).

### Layout facts (x86-64 gcc 13 unless noted)
SysV bit-fields do not straddle their type's alignment unit.  Unnamed and
zero-width bit-fields do not affect struct alignment on x86-64 but do on
AArch64 (`{char c; int :4; char d;}` 3 vs 4).  `{char a; long long :0;
char c;}` is 9/1.  `packed {char; int}` is 5; an `aligned(8)` member of a
packed struct still counts (16).  ms_struct `{char a; int b:4; char c;}`
is 12.  `__int128` and `long double` 16/16.  `{int n; char d[];}` is 4.
i386: `long long`/`double` align 4 in structs, `long double` 12/4.
Win64: `long` 4, `long double` = double.

### Constants
gcc accepts `(int)&((T*)0)->m`, `(long)&x`, `"abc"+1` as initializer
constants (address constants).  Host `long double` on x86-64 matches
gcc's x87 folding bit for bit; other targets need softfloat later.

### Speed
Check <= parse time (Carbon measures check at 2.5-3x parse); parse+check
well under `gcc -fsyntax-only` (corpus 2.4 s vs 6.3 s).  Stretch: 1.3x
parse.

### Testing
Layout: random structs (after gcc `struct-layout-1_generate.c`), gcc's
answers emitted as `_Static_assert` sizeof/offsetof checks; clang
`-fdump-record-layouts` for bit-fields.  Verdict parity on gcc.dg and
the corpus mutants (the "semantic" bucket of `parse_mutate.py` should
reach zero).

### Steps
- **P2a** types, declarations, layout: Target, type table, check pass
  skeleton, declarator marker, `sizeof`/`_Alignof`/`_Static_assert`,
  `--dump-types`, layout parity.
- **P2b** expressions and conversions.
- **P2c** constant expressions and initializers.
- **P2d** statements and function-level checks (labels, switch, return).
- **P2e** summaries and read sets for P3.
- **P2f** parity sweep, speed and memory.

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
1. **P1** (done) tree format, full parser without incrementality, brace
   recovery, `cereal parse [--dump] [--cells]`, `-fsyntax-only`;
   Jourdan–Pottier; gcc parity, on real and mutated code.
2. **P2** scopes, types, summaries, type-checking diagnostics (P2a-f
   above).
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
