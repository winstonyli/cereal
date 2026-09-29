# cereal — architecture

cereal is a C99 toolchain, itself written in C99, whose first-class feature is
static analysis of the preprocessor. The data model is designed so that an LSP
server can treat macros exactly like variables and functions (definition,
references, hover, rename, completion, semantic tokens).

## Decisions

| Area | Decision | Why |
|---|---|---|
| Implementation language | C99 + POSIX (`-std=c99 -pedantic`) | The goal is self-hosting: cereal must be able to preprocess (and later compile) itself. |
| Build | Plain POSIX-ish `Makefile` | No dependencies; `make CC=./cereal` later. |
| Memory | Arenas per translation unit, plus an identifier interner | The whole TU is freed in bulk. The LSP rebuilds one arena per document on each edit. |
| System headers | Host headers only, mimicking the host GCC | `tools/probe-host.sh` bakes GCC's include dirs, its `-std=c99` predefines and its `__has_attribute`/`__has_builtin` answers into `build/gen/host_config.c`. |
| Milestone 1 | Preprocessor (§5.1.1.2 phases 1–4, §6.10) + analyzers + index | |
| Milestone 2 | LSP server over the index (JSON-RPC/stdio) | |
| Priority | Compile time; huge generated files | "No such thing as overengineering." |
| Token core | Streaming lexer over mmap'd input, compact 16-byte tokens / SoA buffers, expansion buffers recycled per top-level expansion (constant memory), SIMD scanning behind `#ifdef` with a scalar C99 fallback | Baseline on a 35 MB generated file: 21.9 s and 3.5 GB peak, against GCC's 3.0 s and 0.5 GB. The cause is eager lists plus retained expansion copies. |
| Parallelism | pthreads, both within a TU (parallel lexing/skeletons of included files, per-function parse→check→codegen) and across TUs (one driver process, work-stealing pool); deterministic output | One giant generated file must scale too. |
| Caching | Content-addressed on-disk cache of per-header results (tokens, macro-table deltas, later decls/proofs), keyed by content hash + the macro state they depend on; shared by CLI, LSP and build daemon | |
| Backend | Own SSA IR → direct x86-64 machine code → ELF `.o` (no assembler); a fast baseline path plus an optimizing path over the same IR; AArch64 later | Compile time. |
| Verification | Built-in, tiered: syntactic → abstract interpretation → permission checking → built-in DPLL(T) solver; deterministic fuel budgets; per-function, cached, parallel (see `docs/SPECS.md`) | Deterministic, self-contained. |
| UB policy | Per function via `#verify strict\|checked\|off`. Strict: UB the prover can't rule out is an error. Checked (default): trap inserted at that point only, and no check where proven. | |
| Spec syntax | Custom directives + `#pragma cereal` spelling; details under discussion in `docs/SPECS.md` | |

## Pipeline

```
SrcMgr ──► Lexer ──► Preprocessor ──► token stream ──► (-E printer | future parser)
 files      pp-tokens    │  directives, expansion, #if eval
 line tables             ▼
                      PPListener callbacks ──► analyzers (hygiene, cond, include)
                                          └──► Index (LSP model)
Skeleton (per file, static) ─► config-space analysis (cond) and guard checks
```

### Source locations and text
The source manager reserves one 4 GiB virtual-address region, and a `SrcLoc`
is a 32-bit offset into it, so **the text at any location is `region + loc`**.

- Files are mmapped (≥ 64 KiB) or read into page-aligned slots, each followed
  by zero padding, so scanners never bounds-check.
- Synthesized spellings (`##`, `#`, builtins, `_Pragma`, cleaned splices) are
  appended to *scratch* chunks in the same space.
- Line tables are built lazily. `-E` uses a forward-moving line cursor
  instead of per-token lookups.

### Tokens
A `Tok` is a 16-byte value with no pointers:
`kind, punct, flags, loc, len, aux`, where `aux` is an identifier id or the
scratch location of the spelling. Tokens are copied freely: listeners
receive views and may keep the values.

### Lexer
The lexer streams on demand.
- **Fast path:** byte-class tables and tight loops, used whenever a token
  contains no line splice or trigraph.
- **Slow path:** handles phases 1–2 character by character, only for tokens
  that need it.
- **Skip scanner:** skipped `#if` groups are crossed by a scanner that
  produces no tokens and only looks for `#` at the start of a line.

### Expansion engine (GCC/Clang model)
Each invocation's replacement is built in a pooled token buffer and pushed
as a **context**. The main loop reads from the top context, and an exhausted
context is popped and its buffer recycled, so memory is bounded by the
deepest expansion rather than by file size.

Recursion is prevented by *disabling* a macro while its context is live and
*painting* (`TF_NOEXPAND`) any identifier read while its macro is disabled.
Arguments are pre-expanded lazily in a sub-stream behind a *barrier*
context. `#if`, `#include` and `#line` operands use the same mechanism.

### Provenance (leveled)
- `TRACK_NONE` (`-E` without listeners): each context carries only the
  outermost call site, which `-E` line placement and `__LINE__` use.
- `TRACK_EXPANSIONS` (lint, index, LSP): every invocation gets an `Expansion`
  record: macro, name/end locations, the parent expansion the name token
  came from, the root and the depth.

Tokens carry cheap origin flags (`TF_ORIGIN_BODY`, `TF_ORIGIN_ARG`,
`TF_PASTED`, `TF_SYNTH`). Diagnostic expansion notes come from the live
context stack.

### Listener API (PP callbacks)
The preprocessor emits `on_file_enter/exit`, `on_include`, `on_define`,
`on_undef`, `on_expand` (with the raw argument lists), `on_macro_ref` (`#ifdef`,
`defined`), `on_cond`, `on_skipped` and `on_pragma`. Analyzers and the index
are independent listeners. Nothing downstream re-parses directives.

### Skeleton pass
Configuration-space analysis must also see code in *inactive* regions. The
skeleton pass re-lexes each user file once and builds its conditional tree
(with `#define`/`#undef`/`#include` positions) without evaluating anything.
The `cond` analyzer turns each condition into a boolean formula over atoms
(`defined(X)`, opaque relational atoms, and so on). It then decides by
enumeration whether each branch is dead in *every* configuration or
redundant given the conditions around it.

### Index (LSP model)
The index is a listener that records:
- every macro definition, with its name range, parameter ranges and body;
- references: expansions, `#ifdef`/`defined`, `#undef`, and references
  *synthesized by `##`* (flagged as not safely renamable);
- parameter references inside bodies (for rename and go-to-definition within a `#define`);
- expansions with their expanded text (for hover);
- inactive regions, conditional blocks (for folding) and include links;
- per-inclusion checkpoints `(offset → event seq)`, so the set of macros
  **visible at any position** can be answered exactly (for completion and
  go-to-definition when the same name is defined several times).

The index is exposed as a C API plus `cereal index --json` and
`cereal query {def,refs,hover,visible,expand,callers,callees,deps}` for
tests. The LSP will call the C API.

### Macro graph
`src/mgraph.[ch]`: nodes are definitions (versions); edges go to the
*names* a replacement list mentions, resolved at the version where the
expansion happens. A definition is *open* when `##` may form a name that
no token spells. The graph is immutable once built:
- `mgraph_closure(names, version)`: the dependency set of some text,
  including names that are not macros (defining one later changes the
  result), plus whether an open definition was reached. This is the basis
  for header memoization and incremental re-preprocessing;
- `mgraph_cycles(version)`: mutual recursion (SCCs, iterative Tarjan),
  reported by `-Wmacro-recursion`;
- call hierarchy: static edges (live ranges overlap) plus the edges
  observed in the recorded expansions, with calls through arguments
  attributed to whoever spelled the argument.

Soundness is checked, not assumed: `cereal index --check-graph` verifies
that every expansion under a file-level invocation lies in the closure of
the invocation's name and argument identifiers (unless open). The suite
runs it on every input and on generated programs (`tests/fuzz_graph.py`).

## Analyses (milestone 1)

- **Hygiene** (`-Wmacro-*`): unparenthesized parameters or bodies,
  multi-statement bodies without `do{}while(0)`, trailing semicolons, arguments
  with side effects evaluated several times (checked at each call site),
  reserved or keyword macro names, and invalid `##`/`#` results.
- **Conditional**: branches that are dead in any configuration, conditions that
  are redundant or constant, `-Wundef`, `#ifdef` typos with did-you-mean,
  `#endif` label mismatches, and `defined` produced by expansion.
- **Include**: the include graph, missing or mismatched include guards, guard
  collisions between files, duplicate includes, cycles, includes with no
  effect (for headers that contain only macros), and unused macros.

## Conformance

C99 is the default. GNU preprocessor extensions that the host headers need
(`#include_next`, `#warning`, `, ## __VA_ARGS__`, `__has_include`,
`__has_attribute`, `__COUNTER__`) are supported and reported with `-pedantic`.
The differential tests compare the token streams of `cereal -E` and
`gcc -std=c99 -E`.

## Source map

| File | Role |
|---|---|
| `src/common.[ch]` | arena, vectors, string buffers, identifier interner |
| `src/srcmgr.[ch]` | files, global location space, line tables |
| `src/lex.[ch]`, `src/token.h` | phases 1–3 pp-token lexer; tokens and provenance |
| `src/pp.[ch]` | include stack, directives, conditionals, guard detection |
| `src/ppexpand.c` | macro replacement, `#`/`##`, builtins, `_Pragma`, `#pragma` |
| `src/ppexpr.c` | `#if` evaluation (intmax_t/uintmax_t, §6.10.1) |
| `src/ppout.c` | `-E` printer with linemarkers |
| `src/skel.[ch]` | static per-file directive skeleton |
| `src/analysis/` | hygiene, cond (configuration space), include analyses |
| `src/index.[ch]` | LSP model and queries |
| `src/mgraph.[ch]` | macro dependency graph: closures, cycles, call hierarchy |
| `src/par.[ch]`, `src/plan.h` | two-phase parallel `-E` |
| `src/thread.[ch]`, `src/intern.[ch]` | pool, atomics; concurrent interner (spellings only; reference counted, so builds can share one) |
| `src/macrotab.[ch]` | per-build macro state by identifier id: definitions, history, poisoning |
| `src/cell.[ch]`, `src/hash.h` | cell cache: phase-B results reused across builds, stored relative to their cells (PARALLEL.md "Cells") |
| `src/c/` | the C parser: keywords (`ckw.h`), post-order syntax trees (`ast.[ch]`), typedef scopes (`scope.[ch]`), recursive descent (`parse.[ch]`); PARSER.md |
| `src/toks.[ch]` | a cell build's token stream without storing it: per-cell token counts and hashes, regeneration by re-running cells (PARSER.md) |
| `src/lsp/` | language server: framing, positions, configuration, builder, requests (docs/LSP.md) |
| `src/json.[ch]` | JSON writer (buffered) and DOM reader |
| `src/driver.[ch]`, `src/main.c` | options, translation-unit setup, CLI |
| `tools/probe-host.sh` | host compiler probe (generates `build/gen/host_config.c`) |
