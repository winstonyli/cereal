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
| Later | Parser / semantic analysis / backend | The backend choice is deferred. |

## Pipeline

```
SrcMgr ──► Lexer ──► Preprocessor ──► token stream ──► (-E printer | future parser)
 files      pp-tokens    │  directives, expansion, #if eval
 line tables             ▼
                      PPListener callbacks ──► analyzers (hygiene, cond, include)
                                          └──► Index (LSP model)
Skeleton (per file, static) ─► config-space analysis (cond) and guard checks
```

### Source locations
`SrcLoc` is a 32-bit offset into one global address space. Each physical file
gets a contiguous range `[base, base+size]`, and 0 means invalid. Files are
loaded once and shared by every inclusion, so every location maps to exactly
one `(file, offset)`. That mapping is what an LSP needs. The inclusion
context lives in the include stack and is captured with each diagnostic.

The lexer works on the raw buffer and handles line splices and trigraphs
inline, so token locations are always raw buffer offsets (an exact LSP range
is `loc .. loc + rawlen`).

### Provenance (macro-expansion tracing)
Every token carries:
- `loc`: the **spelling** location, where its characters are written.
- `prov`: a chain of `Prov` steps describing how it got there:
  - `PROV_BODY`: copied from a macro's replacement list (`loc` is inside the `#define`).
  - `PROV_ARG`: substituted for a parameter. It records the parameter's
    occurrence in the body, and `inner` is the provenance the argument token
    had at the call site.
  - `PROV_PASTE` / `PROV_STRINGIZE` / `PROV_BUILTIN`: synthesized tokens.

Each step points to an `Expansion` (the macro, the invocation range, and the
provenance of the macro-name token at the call site, which gives the parent
expansion). This equals clang's SourceManager macro expansion entries, with
enough information to answer "where did this token come from" and to
render expansion notes under diagnostics.

### Expansion algorithm
This is Prosser's hide-set algorithm (the reference algorithm behind C99 §6.10.3.4),
operating eagerly on linked token lists allocated in the arena. For
function-like macros, the hide set of the result is
`(HS(name) ∩ HS(rparen)) ∪ {macro}`.

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
`cereal query {def,refs,hover,visible}` for tests. The LSP will call the C API.

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
