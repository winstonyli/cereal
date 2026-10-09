# Language server

`cereal lsp` speaks the Language Server Protocol over stdin/stdout. Its
point is to give macros what variables and functions get from other
tools: navigation, rename, call hierarchy, hover with the expansion,
diagnostics from the macro analyses and, for files up to 4 MiB of
sources, the compiler's (parse and check, as `-fsyntax-only` gives).
It also runs fast on huge generated files.

## Decisions

| Question | Decision |
|---|---|
| Transport | stdio (what every editor spawns). The protocol layer is independent of it, so a socket or daemon can be added if measurements ever justify it. Shared warm state comes from the on-disk cache (PARALLEL.md step 8) instead. |
| Edits | Memoized recomputation, staged. Stage 1: a full rebuild per edit with snapshots and cancellation. Stage 2 (here): phase-B results cached per *cell* (a run of text between clean, content-defined boundaries), stored relative to the cell and keyed by text, read set (misses included) and entering state; the index is kept in the cells and queries walk them (Roslyn's green/red split, clangd-style layers); checked against full rebuilds by a differential fuzzer (PARALLEL.md, "Cells"). Next: phase-A checkpoints. Patching data structures in place was rejected: it is fragile, and no production C/C++ engine does it (research notes below). |
| Features | Full index parity, plus macro-specific extras. |
| Configuration | `compile_commands.json` (root or `build/`), then `.cereal` files from the workspace root down to the file's directory. Headers and files without an entry take the flags of the entry with the nearest path. |

## Architecture (src/lsp/)

| File | Role |
|---|---|
| `rpc.c` | `Content-Length` framing; one output lock |
| `pos.c` | `file://` URIs; positions in UTF-8 (when the client offers `positionEncodings: ["utf-8"]`) or UTF-16 |
| `config.c` | compilation database, shell splitting, `.cereal`, per-file `Options` |
| `server.c` | protocol loop, documents, units, the builder thread, snapshots |
| `features.c` | requests, diagnostics (macro phase and compiler, merged), inactive regions |
| `../c/frontend.c` | parse and check one TU (shared with `cereal check`); the second build phase |

- **Units:** a unit is a translation unit the server keeps built: a
  source file, or a header opened on its own. When a built unit is seen
  to include such a header, the header moves to that unit and gets its
  features with the includer's flags, as it is really compiled.
- **Builder:** one builder thread. An edit bumps the unit's wanted
  generation and cancels its running build: `PP.cancel` is an atomic flag
  checked once per token, inherited by parallel workers. The builder then
  freezes the editor buffers into an *overlay* (the source manager reads
  it before the disk) and builds preprocessing, analyses, index and macro
  graph with the two-phase parallel runner. A build that was not cancelled
  becomes the unit's snapshot.
- **Interner:** a unit's builds share one identifier interner, so ids
  stay stable across edits (the key the result cache needs). Macro state
  is per build (`MacroTab`). The interner is replaced when it grows past
  twice what a fresh build needed.
- **Cells:** each unit keeps the cell cache of its last build (large
  files only: those that take the parallel path). A change of flags
  empties it; a cancelled build leaves it as it was.
- **Snapshots** are immutable and reference counted. Requests run on the
  protocol thread against the latest complete snapshot and never wait for
  a build; only the first build of a unit is waited for. The exception is
  definition and declaration, which wait up to 1.5 s for the C symbol
  index of the newest edit (see "C symbol index").
- **Diagnostics** are published after every build for the open documents
  the unit covers. Errors in headers that are not open appear on the
  `#include` that leads to them.
- **Compiler diagnostics** (the same as `cereal -fsyntax-only` for the
  file, with its compile command's flags including `-std`) come from a
  second phase of the build, see below.

## Compiler diagnostics (the second build phase)

After the macro snapshot has been installed and its diagnostics
published, the builder thread (still marked busy, so `waitIdle` waits)
runs `frontend_run` (`src/c/frontend.c`, the function `cereal check`
uses too) over a **fresh TU of its own**: a second, sequential
preprocessor pass (unlike gcc and the command line, a missing include is
not fatal here, so that the symbol index covers the whole file; compiler
diagnostics after the first missing include are dropped, as gcc never
reports them), the parser, and the checker, reading the same frozen overlay and the
unit's cancel flag. The snapshot's TU cannot be reused: for small files
the preprocessor was consumed by the macro index, and large files keep
cells, not tokens. The second pass is 30 to 45% of the phase (`-E` against
`check`: main.c 0.04 s / 0.09 s, cexpr.c 0.07 s / 0.22 s, zstd.c 0.25 s /
0.60 s).

- **Merge:** the second publication replaces the first for each open
  file with the macro phase's diagnostics followed by the compiler's,
  minus any with the same range and message (a missing `#include` is
  reported by both). Until the phase ends, or if it is skipped or
  cancelled, only the macro phase's are shown, so compiler diagnostics
  disappear for the length of a check after each edit.
- **Cancellation:** an edit sets the unit's cancel flag; the preprocessor
  sees it at the next token and the parse loop ends at the next unit, so a
  running check stops within a declaration. A cancelled run is dropped
  without publishing (a 4 MB table cancelled 0.46 s into a check stopped
  at the edit). Shutdown cancels too.
- **Stale results:** the result is published under the server lock only if
  the unit's wanted generation is still the one the overlay was captured
  at; an edit bumps it under the same lock, so diagnostics always match
  the buffers they were computed from. Not published: a cancelled or
  unopenable run.
- **Size limit:** units whose sources (main file and every header read,
  system headers included) total over 4 MiB are not checked
  (`CEREAL_LSP_CHECK_MAX`, bytes, overrides; `CEREAL_LSP_STATS` logs
  `no check`). Measured (nice, WSL2) with the check phase on against off:
  peak RSS grows by 9 MB for zstd.c (2.2 MB: 21 to 30 MB), 16 and 21 MB
  for sqlite3.c and uvloop's loop.c (9 MB: 79 to 95 and 150 to 171 MB), but by
  194 MB for a dense 4 MB single-initializer table (48 bytes of parser
  tokens per source byte) and 420 MB at 9 MB, which set the limit. The
  macro phase peaks at 830 MB on the 35 MB file; a unit over the limit
  never pays more than its macro phase.
- **What is retained:** the TU, the parser's tokens and the checker are
  freed when the phase ends; only the C symbol index (below) is kept, on
  the snapshot.
- **Which units:** every unit except a header opened on its own (no
  compile command, not yet included by a unit). Once a unit includes such
  a header, its compiler errors show in the header's own buffer, through
  the includer's check.
- **Time** (phase only, after the macro snapshot): 33 KB main.c 0.05 s,
  248 KB cexpr.c 0.06 s, zstd.c 0.34 s (2.2 MB), a dense 4 MB table
  1.7 s. sqlite3.c (9 MB) took 1.1 s and loop.c 3.4 s before the limit.
- **Test:** `tests/lsp/diag` (undeclared identifier and type error, two
  quick edits whose final diagnostics must be the last text's, a missing
  include shown once). Whether a quick edit really *cancelled* a check
  is timing-dependent and not asserted; the stale-result rule is.

## C symbol index (B2, phase 1)

The check phase also records a symbol index (`src/c/csymidx.c`; design
in B2_DESIGN.md): every declaration, definition and use of a function,
variable, parameter, typedef, enumerator, field, label and tag, as
events (file, offset, length, role DECL/DEF/REF, macro flags) pointing at
decls (kind, name, linkage). The checker calls about 20 cheap hooks
(`csx_*`) that do nothing unless the index was asked for
(`CheckOptions.symidx`), so `cereal check` and gcc parity are unchanged.
The frozen `CIndex` moves onto the snapshot when the check publishes and
is freed with it.

- **Locations:** an event inside a macro expansion is placed at its
  spelling when the token came from an argument, else at the invocation
  (`expansion`); one presented in a system header is dropped, and a system
  decl used by user code gets one lazy event at its declaration. Each file
  carries a hash: a file whose text differs from the snapshot's is marked
  stale and its events are not answered. Files absent from the snapshot
  are skipped (no line table to convert offsets).
- **Queries:** definition answers DEF events, else DECL; declaration
  answers DECL, else DEF (`declarationProvider`). The macro index is asked
  first: an expanded macro at the cursor wins; a name the macro index only
  knows by its plain identifier (a macro since `#undef`'d, `weak`), or
  nothing, goes to the C index.
- **Waiting (decision D1):** right after an edit the snapshot's index is
  not ready yet. definition and declaration wait (`cond_timedwait`, at most
  1.5 s) while the newest edit has no snapshot or its check is pending,
  then answer from what is there: the macros alone if no index published.
- **No index:** units over the check size limit, headers opened on their
  own, cancelled checks. C queries then return nothing; macros still work.
- **Size (measured, x86_64):**

  | Unit | Events | Decls | Files | CIndex |
  |---|---|---|---|---|
  | src/main.c | 7,291 | 3,269 | 38 | 0.20 MB |
  | src/c/cexpr.c | 24,828 | 5,153 | 30 | 0.52 MB |
  | zstd.c (2.2 MB) | 65,898 | 12,717 | 15 | 1.37 MB |

  16 bytes per event (as estimated) and about 25 per decl before hover
  strings (phase 3; `--dump-types` lines average 58 bytes, so about 83 per
  decl with hover, against the design's 60). `cereal check` peak RSS on
  zstd.c grows from 12.2 to 14.4 MB; time is within noise (cexpr.c best of
  7: 0.079 s off, 0.080 s with `--verify-symbols`).
- **Command line:** `cereal check --dump-symbols FILE` prints the events
  and the decl table; `--verify-symbols` checks that every identifier the
  checker resolved has its event and that the index is well formed
  (sorted, CSR consistent, every decl declared), excusing lines with a
  diagnostic, and prints a `symbols:` summary line.

## Capabilities

definition and declaration, references, hover (definition, body, and
what the invocation under the cursor expands to), completion (the macros
visible at the cursor), document symbols, semantic tokens (`macro`,
`parameter`; `declaration`; whole file, a range, or a delta against the
last whole-file result), folding (`#if` blocks, multi-line
`#define`s), rename with prepareRename, call hierarchy (incoming and
outgoing; static edges plus observed expansions), and signature help for
function-like macros (read from the editor text, so it works while the
invocation is still being typed).

- **Rename is refused** when any use of the name is formed by `##`
  (renaming could not follow it), or when the macro is predefined or lives
  in a system header.
- **Extensions:**
  - `textDocument/inactiveRegions` (clangd's notification), sent when the
    client declares `inactiveRegionsCapabilities`;
  - `cereal/expandMacro` (position: the invocation's full expansion);
  - `cereal/waitIdle` (answers when no build is queued or running; a
    barrier for tests).
- **C names:** definition and declaration (above). References, hover and
  rename for C names are B2 phases 2 to 4.

## Tests

`tests/lsp_session.py` runs scripted sessions (`tests/lsp/*.json`) and
compares transcripts with golden files. Scripts can create files (a
compilation database with absolute paths), open workspace files, wait
for notifications, and use `waitIdle` barriers so asynchronous builds
stay deterministic. An `{"env": {...}}` step sets the server's
environment (`tests/lsp/csym_nocheck` uses it to turn the check off).
`tests/lsp/csym` covers C definition and declaration for every kind,
shadowing, macro-vs-C precedence, D1 and D2. The sessions are also run under ThreadSanitizer and
AddressSanitizer/UBSan (set `LSP_STDERR` to collect reports).

## Measurements (35 MB macro_heavy.c, 4 cores)

| Step | Stage 1 | Now |
|---|---|---|
| open to diagnostics | 3.1 s | 1.85 s |
| edit to diagnostics (a line typed mid-file) | 3.1 s | 0.2 s |
| hover | 0.1 s | 0.02 s |
| call hierarchy (prepare, incoming, outgoing) | | 0.01 s each |
| references to a macro used 400k times (75 MB of results) | | 1.3 s in the server, 5 s at the client |
| semantic tokens, whole file (4M integers) | 0.6 s | 0.9 s (0.6 s again for the same build) |
| semantic tokens, 40 visible lines | | 0.02 s |
| semantic tokens delta after an edit | | 1.2 s to compute, 10 integers sent |

Normal files take milliseconds. Of an edit, the build is about 0.11 s:
phase A 0.06 s, cell lookups 0.02 s, the rerun cell 0.015 s. References
used to take 145 s there: two deduplication passes compared every
location with every earlier one (now hashed). Semantic tokens for a
range read only the cells and definitions meeting it
(`index_range_refs`); whole-file results are kept per document and
build, so a repeated request is only serialization and a delta is one
edit (common prefix and suffix). A delta still recomputes the whole
file; editors ask for the visible range first. `CEREAL_LSP_STATS=1` prints
the server's timings (and `CEREAL_PAR_STATS=1` the runner's).
`-fparallel=on` in `.cereal` forces the parallel path and cells for
files of any size (the tests use it).

## Research notes (why memoization, not patching)

- clangd, Visual Studio (EDG) and CLion cache the header prefix
  (preamble/PCH) and fully re-parse the rest of the file per edit.
  clangd's preamble patching handles `#include` but not `#define`.
- tree-sitter patches trees incrementally but has no typedef tracking and
  guesses at macros, so it is syntax-only for C.
- rust-analyzer (salsa) and Zig memoize per item behind a summary
  "firewall", with early cutoff, and verify incremental results against
  full rebuilds.

The pitfalls they documented all have a place in the cache design:
- lookups that fail (ccache misses newly created headers);
- context read past a unit's end (a swift-syntax bug);
- identity tied to position (rust-analyzer's stale ids);
- one variant per header (Eclipse CDT's wrong typedefs).
