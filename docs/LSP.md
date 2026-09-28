# Language server

`cereal lsp` speaks the Language Server Protocol over stdin/stdout. Its
point is to give macros what variables and functions get from other
tools: navigation, rename, call hierarchy, hover with the expansion,
diagnostics from the macro analyses. It also runs fast on huge generated
files.

## Decisions

| Question | Decision |
|---|---|
| Transport | stdio (what every editor spawns). The protocol layer is independent of it, so a socket or daemon can be added if measurements ever justify it. Shared warm state comes from the on-disk cache (PARALLEL.md step 8) instead. |
| Edits | Memoized recomputation, staged. Stage 1 is here: a full rebuild per edit with snapshots and cancellation. Next is per-segment result caching keyed by text, read set (misses included) and entering state, with early cutoff and differential checks against full rebuilds. Patching data structures in place was rejected: it is fragile, and no production C/C++ engine does it (research notes below). |
| Features | Full index parity, plus macro-specific extras. |
| Configuration | `compile_commands.json` (root or `build/`), then `.cereal` files from the workspace root down to the file's directory. Headers and files without an entry take the flags of the entry with the nearest path. |

## Architecture (src/lsp/)

| File | Role |
|---|---|
| `rpc.c` | `Content-Length` framing; one output lock |
| `pos.c` | `file://` URIs; positions in UTF-8 (when the client offers `positionEncodings: ["utf-8"]`) or UTF-16 |
| `config.c` | compilation database, shell splitting, `.cereal`, per-file `Options` |
| `server.c` | protocol loop, documents, units, the builder thread, snapshots |
| `features.c` | requests, diagnostics, inactive regions |

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
- **Snapshots** are immutable and reference counted. Requests run on the
  protocol thread against the latest complete snapshot and never wait for
  a build; only the first build of a unit is waited for.
- **Diagnostics** are published after every build for the open documents
  the unit covers. Errors in headers that are not open appear on the
  `#include` that leads to them.

## Capabilities

definition and declaration, references, hover (definition, body, and
what the invocation under the cursor expands to), completion (the macros
visible at the cursor), document symbols, semantic tokens (`macro`,
`parameter`; `declaration`), folding (`#if` blocks, multi-line
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

## Tests

`tests/lsp_session.py` runs scripted sessions (`tests/lsp/*.json`) and
compares transcripts with golden files. Scripts can create files (a
compilation database with absolute paths), open workspace files, wait
for notifications, and use `waitIdle` barriers so asynchronous builds
stay deterministic. The sessions are also run under ThreadSanitizer and
AddressSanitizer/UBSan (set `LSP_STDERR` to collect reports).

## Measurements (35 MB macro_heavy.c, 4 cores)

| Step | Time |
|---|---|
| open to diagnostics | 3.1 s |
| edit to diagnostics (full rebuild) | 3.1 s |
| hover | 0.1 s |
| semantic tokens, whole file (4M integers) | 0.6 s |

Normal files take milliseconds. The rebuild is what stage 2 removes;
`semanticTokens/range` and deltas will cut the token payload.

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
