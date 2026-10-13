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
| Configuration | `compile_commands.json` (root or `build/`), then `.cereal` files from the workspace root down to the file's directory. Headers and files without an entry take the flags of the entry with the nearest path. An entry's flags resolve against its `directory` (`Options.cwd`, shared with `check --compile-commands`: `compdb.c`); `.cereal` path flags are made absolute against the `.cereal` file. Unknown flags are skipped. |

## Architecture (src/lsp/)

| File | Role |
|---|---|
| `rpc.c` | `Content-Length` framing; one output lock |
| `pos.c` | `file://` URIs; positions in UTF-8 (when the client offers `positionEncodings: ["utf-8"]`) or UTF-16 |
| `config.c` | database lookup, `.cereal`, per-file `Options` (the database, shell splitting and `entry_options` are `../compdb.c`) |
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
  the requests that read the C symbol index, which wait up to 1.5 s for
  the newest edit's snapshot and, depending on the request, its check
  (see "C symbol index", Waiting). The snapshot's C index is not
  immutable: it is the previous one carried over the edit until the
  check publishes its own, so requests hold a reference while they answer.
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
  reported by both). Every publication carries the document's `version`
  when the client sent one (didOpen, didChange).
- **Carried diagnostics (no flicker):** until the check ends, the macro
  phase's publication also shows the previous snapshot's compiler
  diagnostics, moved through the same text edit as the carried index
  (B3_DESIGN.md 12.8). A diagnostic is stored as file offsets (range, and
  the path and offset of each note) and the edit of each file is the
  difference of its two texts, widened to whole identifiers. One whose
  range or any note meets the edit's span (inclusive at both ends), or
  whose file is gone, is dropped; the others are shifted, so errors below
  the edit stay visible, at their new lines, while typing, and an edit of
  an included header moves the notes pointing into it. A diagnostic is
  carried only if the check read the same text of every file it names as
  the snapshot (otherwise its offsets mean nothing). They are rendered
  against the new snapshot's line table, and stored on it, so a chain of
  cancelled checks keeps carrying them. If the
  check fails (below) they are withdrawn with a republication; a
  superseded check leaves them to the next snapshot.
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
  never pays more than its macro phase. Such a unit's main file shows
  one Information diagnostic at line 1 ("not checked for compiler
  errors: its sources total X, over the limit of 4.0 MiB
  (CEREAL_LSP_CHECK_MAX)", sizes in MiB, KiB or bytes); `cereal check` and the command line have no
  limit and no notice.
- **fatal() in a phase:** `fatal()` normally exits. In the server the
  builder thread runs each phase (build, check) under a thread-local trap
  (`FatalTrap` in common.h, setjmp/longjmp), and pool jobs run under one
  too (`ThreadPool.trap_fatal`, set only by `cereal lsp`): the first
  failing job's message is re-raised by `group_wait` in the builder. The
  phase is dropped, the message goes to stderr and to the client
  (`window/logMessage`, type 1), a failed build also gives the unit a new
  interner and cell cache, and the server lives on; the next edit builds
  again. The partial state is leaked, not freed (it may be
  inconsistent). A `fatal()` while a mutex is held (counted per thread
  in `mutex_lock`) still exits, as the lock could not be released.
- **Fault injection:** `CEREAL_FAULT=site[:N]` fires at the Nth pass of
  a site (default 1), for tests: `par-worker` (`fatal()` at the start of
  a phase-B worker job), `lsp-check` (`fatal()` at the start of a check),
  `pp-halt-in-if` (a cancel landing just before an `#if` expression is
  expanded).
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
  `tests/lsp/carry` (an insertion between two errors keeps both, the
  lower one moved; an edit above both keeps both; an edit of the
  identifier of one drops that one and keeps the other; a didChange without
  a version publishes none), `carry_hdr` (checks held; an edit of an
  included header only: the error in the includer is kept and its note into
  the header moved, definition and hover through the carried index), `skip` (the
  notice with `CEREAL_LSP_CHECK_MAX=100`, gone once the file shrinks under
  it), `fault` (a `fatal()` in a pool worker of the first build, then a
  normal edit and hover) and `fault_check` (a failed check withdraws the
  carried diagnostic and logs; the next check works). run.sh also runs
  `cereal -E` with `pp-halt-in-if` under a 6 GB memory cap (the Round 189
  crash, below).
- **Cancel inside an expansion (Round 189):** a cancel sets
  `PP.halted`, after which `pp_read_raw` ended the stream with a lexer EOF
  and skipped barrier contexts. `expand_into` (an `#if` expression, a
  macro argument) reads until its barrier, so it pushed EOF tokens until
  the token buffer gave out ("token buffer too large" after about 90 s of
  paging on uvloop's loop.c). A halted stream now still returns
  `SRC_BARRIER` while a barrier context is open.

## C symbol index (B2)

The check phase also records a symbol index (`src/c/csymidx.c`; design
in B2_DESIGN.md): every declaration, definition and use of a function,
variable, parameter, typedef, enumerator, field, label and tag, as
events (file, offset, length, role DECL/DEF/REF, macro flags) pointing at
decls (kind, name, linkage). The checker calls about 20 cheap hooks
(`csx_*`) that do nothing unless the index was asked for
(`CheckOptions.symidx`), so `cereal check` and gcc parity are unchanged.
The frozen `CIndex` moves onto the snapshot when the check publishes and
is freed with it (replacing the carried index, below).

- **Locations:** an event inside a macro expansion is placed at its
  spelling when the token came from an argument, at the invocation
  (`expansion`) when the name was formed by `##`, else (a name written in
  a `#define` body) at the `#define`; one presented in a system header is dropped, and a system
  decl used by user code gets one lazy event at its declaration. Each file
  carries a hash: a file whose text differs from the snapshot's is marked
  stale and its events are not answered. Files absent from the snapshot
  are skipped (no line table to convert offsets).
- **Queries:** definition answers DEF events, else DECL; declaration
  answers DECL, else DEF (`declarationProvider`). references answers every
  event of the entity in this unit (its declarations too unless
  `includeDeclaration` is false); documentHighlight the same events in the
  requested file only, Write for a declaration or definition and Read for
  a use. One location is given once (a declaration rather than a use
  there). Redeclarations are one entity, each struct's fields are their
  own, tags and ordinary names are apart, and shadowing follows C scoping.
  A name written in a macro argument counts where it is written; a token
  of a `#define` body is no use and is left out (its place is the body,
  not the invocation). The macro index is asked first: an expanded macro
  or macro parameter at the cursor wins (references and highlight then
  answer as before; highlight gives the definitions Write and the uses
  Read); a name the macro index only knows by its plain identifier (a
  macro since `#undef`'d, `weak`), or nothing, goes to the C index. The
  selection is shared with `cereal query` (`cindex_select`).
- **Hover** (phase 3) shows, over the name, the checker's text for the
  entity, copied out as a string while the checker's tables live: the
  `--dump-types` line for functions (`func f: int(const char *, ...)
  static inline defined`), variables (`var f:x: const int static`, a
  block-scope name prefixed with its function), parameters, typedefs
  (`typedef T = TYPE`, the aliased type) and enumerators (`enumconst A =
  4 (int)`); a field's type, record and offset (`field kind: unsigned
  int (struct rect, offset 8 bit 3, width 5)`); a struct or union's
  layout and an enum's enumerators (at most 16, then `... N more`);
  `label f:out`. Texts over 1024 bytes are cut (`...`). Equal texts are
  stored once. A macro expanded at the cursor still answers with its
  definition and expansion; a name with only macro history answers with
  the C text plus "(also a macro name)"; several entities at one place
  list their distinct texts (at most 5). Doc comments are not shown.
  Where neither index answers inside a skipped `#if` group, hover says so
  instead of null: `inactive code (skipped by #ifdef at main.c:12)`, the
  innermost `#if`/`#ifdef`/`#ifndef` around the group as spelled
  (`index_inactive_note`, over `Index.inactive` and `Index.blocks`, the
  ranges the inactiveRegions notification sends); definition and
  references stay empty there.
- **Call hierarchy** (B4a; B4_DESIGN.md 4 and 5): a REF event of a function
  is flagged `CIX_CALL` (flag 64) when it is the callee of a call
  expression (`call_callee`, set by the identifier hook and for an
  implicit declaration; a called field by the field hook). Dedupe ORs the
  flag, the dump prints ` call`, `--verify-carry` compares it, and
  `--verify-symbols` reports `unindexed 'NAME' (call)` for an identifier
  callee with no flagged event. A callee spelled in a `#define` body has
  its event at the `#define`, so the builder also queues an `xcall` (a side
  array, functions only, carried and verified) at the invocation; the
  hierarchy reads both. The caller of a call is the function whose root
  scope holds the site (a file-level variable, per declarator, when it is in
  an initializer; none for a call in a `_Static_assert`). prepare answers a
  strong macro first, else the C function or file-level variable at the
  place (`cindex_site`); a local variable or an unnamed place is null.
  Items carry `data` {c, name, doc}; a follow-up is resolved by position and
  name, then `data.c`, then a scan by name (`c_item_resolve`), against the
  request's document (the E4 fix: `request_uri` prefers `data.doc`, so an
  item in an unopened header resolves against the open unit). Incoming and
  outgoing group the calls by the other function and report the sites in
  the item's file as `fromRanges`. Policies: prepare `CP_POS`, incoming and
  outgoing `CP_WAIT`. `cereal query callers|callees FILE:L:C main.c` prints
  the same groups for a C name (`LOC KIND NAME: SITES`; `no callers`,
  `no callees`, `[declaration only]`, `[implicit]`).
- **Rename** (phase 4; B2_DESIGN.md, "Phase 4 design addendum"):
  prepareRename and rename take a C name when the macro index does not
  answer for it (as above). The edits are the entity's events, one per
  place, and all lie in the unit's main file. A name written in a macro
  argument is edited there (decision D4). The rename is then checked by
  checking the unit again with the edits applied (`src/c/crename.c`): it
  stands only if the two runs give the same events (each naming the same
  entity), decls and diagnostics, up to the moved offsets and the new
  name. That comparison is the conflict check, so it follows the
  checker's own scoping: capture by an inner or outer declaration, a
  redeclaration in the same scope, a duplicate member or label, a
  typedef renamed to a variable used where it is, a `#define` body that
  would now name another entity, and a renamed argument that `##` pastes
  into another name are all refused with the first place that differs.
  Names in other namespaces (a tag against a variable, a field against a
  global, a label against a parameter) are accepted. Also refused,
  before the second check:
  - the index is not the newest edit's ("not ready; retry");
  - several entities at the place, a predeclared or system name, an
    implicitly declared function;
  - a use spelled in a `#define` body or formed by `##`;
  - a use in a header (other units may include it; cross-unit rename is
    B5, decision D3);
  - a new name that is not an identifier, is a keyword in any mode, or
    has any macro history in the unit;
  - an `#include` not found;
  - the old or new name as a word in a skipped `#if` group within the
    entity's scope (its function for a local, parameter or label, else
    the whole unit).

  A refusal is a RequestFailed error with the reason (prepareRename too,
  and now also for macros); prepareRename answers null where there is no
  entity. The two checks run on the builder thread, to keep checks ordered
  beside the edits (the checker's state is per thread): the request is
  handed to the builder as a `DeferredReq` (as the first fix-all is) and
  the protocol thread goes on serving; the builder answers it. If the document changed meanwhile, the
  answer is a ContentModified (-32801) error, the edits being for the old
  text. A request for a stale or unchecked snapshot is refused at once
  ("retry"), as before.
  Not seen by the comparison: an argument the macro also stringizes
  (`#x` spells the new name), `__func__` in a renamed function, names
  in strings (`alias("f")`, `asm` labels) and other units.
- **Carried index (B3_DESIGN.md 12):** when the builder installs a
  snapshot that will be checked, it carries the previous snapshot's index
  over the edit (`cindex_carry`): events and scopes copied with offsets
  shifted past the edit (the difference of the two texts, widened to whole
  identifiers), events overlapping the edit dropped, decls and strings
  shared. It sits on the new snapshot (`cidx_carried`) until the check
  publishes its own index (`Snapshot.cidx` is replaced under the server
  lock; requests take a reference). A chain of cancelled checks carries
  from the carried index (the damage is the hull). A failed check drops the
  carried index (the snapshot then answers with macros only). While it is
  in use the extra index stays alive during the check: index plus copy,
  measured 4.1 MB on zstd.c and 43 MB at worst (B3_DESIGN.md 12.14).
- **Waiting (decision D1, narrowed by carry-over):** right after an edit
  the snapshot's own index is not ready. Every request first waits
  (`cond_timedwait`, at most 1.5 s) for the newest edit's snapshot so that
  positions match; the policy column of the request table in server.c then
  says whether it also waits for the check: `CP_ANY` (completion,
  signatureHelp, documentHighlight, documentSymbol, and semantic tokens
  or inlay hints for a client with the matching `refreshSupport`, the
  policy `CP_VIEW`) only when the
  snapshot has no index at all; `CP_POS` (hover, definition, declaration,
  typeDefinition) also when the request's position is inside the carried
  index's damage; `CP_WAIT` (references, prepareRename, rename, call
  hierarchy follow-ups, and semantic tokens or inlay hints for a client
  without the matching `refreshSupport`) always. After the timeout
  a request answers from what is there: the carried index, or the macros
  alone if there is none. Rename uses only a fresh index: with a carried
  one it is refused ("retry").
- **What the client sees of a carried answer:** completion is
  `isIncomplete: true`; hover adds "(rechecking: its declaration was
  edited)" when the entity's declaration was edited (a lost DECL/DEF event,
  or one on a line the damage touches); a semantic token or inlay hint
  answer made without the snapshot's own index sets that view's bit in the
  unit's `views_stale` (`VIEW_TOKENS`, `VIEW_HINTS`), and when a check then
  publishes, the server sends the client `workspace/semanticTokens/refresh`
  and/or `workspace/inlayHint/refresh` (requests with id `cereal-refresh-N`;
  the client's responses are ignored). The whole-file
  token result is keyed by the index's serial, so a result made from a
  carried index is not reused after the own one publishes. A macro token
  and a carried C token starting at one place: the macro's wins.
- **No index:** units over the check size limit, headers opened on their
  own, cancelled checks. C queries then return nothing; macros still work.
- **Size (measured, x86_64):**

  | Unit | Events | Decls | Files | CIndex without hover | with hover (Round 192) |
  |---|---|---|---|---|---|
  | src/main.c | 7,617 | 3,373 | 39 | 0.21 MB | 0.34 MB |
  | src/c/cexpr.c | 24,921 | 5,215 | 31 | 0.53 MB | 0.68 MB |
  | zstd.c (2.2 MB) | 65,898 | 12,717 | 15 | 1.37 MB | 1.81 MB |

  16 bytes per event (as estimated), about 25 per decl without hover
  texts and 30-39 more with them (not the 58 projected from `--dump-types`
  line lengths: equal texts are stored once; zstd.c's 583 KB of texts are
  7,416 distinct, 429 KB). The worst case is a dense file of short
  distinct declarations: a 4 MiB struct of `int fN;` members (330,869
  fields) retains 29.3 MB (14.4 MB without texts), about 7 bytes per
  source byte. `cereal check` peak RSS on zstd.c grew from 12.2 to 14.4 MB
  with phase 1; time is within noise (cexpr.c best of 7: 0.079 s off,
  0.080 s with `--verify-symbols`; zstd.c with texts at most 8% slower,
  about the noise between rounds).
- **Command line:** `cereal check --dump-symbols FILE` prints the events
  and the decl table (each decl's hover text after ` :: `, newlines as
  `\n`); `--verify-symbols` checks that every identifier the
  checker resolved has its event and that the index is well formed
  (sorted, CSR consistent, every decl declared), excusing lines with a
  diagnostic, and prints a `symbols:` summary line. `cereal query
  def|decl|refs|uses|highlight|hover FILE:L:C main.c` answers a C name with the
  same merge as the server (a second, checking run builds the index;
  `uses` is references without declarations): one line per location,
  `file:line:col ROLE kind name` plus the macro flags (`arg`, `body`,
  `expansion`, `system`), write/read for highlight, and a count line when
  several entities share the place (a `#define` body token); hover prints
  the texts, then "(also a macro name)" for a name with macro history,
  and the inactive-code note where nothing answers in a skipped group.
  `cereal query rename=NEW FILE:L:C main.c` prints `file:line:col NEW`
  per edit, or `cannot rename: REASON` (also for a macro at the place).
  `--dump-symbols` shows a block-scope decl's scope lines (`scope L-L`).

## Capabilities

definition, declaration and type definition, references, document highlight, hover (definition, body, and
what the invocation under the cursor expands to), completion (the macros
visible at the cursor, then the C names in scope there that no visible
macro hides), document symbols (the macros, then the file-level C
declarations with fields and enumerators nested under their record or
enum), semantic tokens (`macro`, `parameter`, `function`, `variable`,
`type`, `enumMember`, `property`, `struct`, `enum`; modifiers
`declaration`, `readonly`, `static`; whole file, a range, or a delta
against the last whole-file result), folding (`#if` blocks, multi-line
`#define`s), parameter-name inlay hints for C calls (below), workspace/symbol
over the open units (below), rename with
prepareRename, call hierarchy (incoming and
outgoing; for macros static edges plus observed expansions, for C
functions and file-level variables the call index), and signature help for
function-like macros and for C functions and function pointers, also
through a typedef (both read from the text, so they work while the call
is still being typed). The C parts follow docs/B3_DESIGN.md.

- **Rename of a macro is refused** when any use of the name is formed by
  `##` (renaming could not follow it), or when the macro is predefined or
  lives in a system header.
- **Extensions:**
  - `textDocument/inactiveRegions` (clangd's notification), sent when the
    client declares `inactiveRegionsCapabilities`;
  - `cereal/expandMacro` (position: the invocation's full expansion);
  - `cereal/waitIdle` (answers when no build is queued or running; a
    barrier for tests);
  - `cereal/holdChecks` `{"hold": bool, "builds": bool}` (test only): while
    held, a check waits at its start (an edit, the release or shutdown ends
    the wait), so a request after an edit is answered from the carried index
    deterministically; with `builds` the builder also waits before each
    build, so that the snapshot stays older than the document. Do not use
    `waitIdle` while held: it waits for the check.
  - `workspace/semanticTokens/refresh` and `workspace/inlayHint/refresh`,
    sent by the server (the only requests it sends) after a check
    publishes, if a token or hint answer lacked the fresh index and the
    client declared the matching `refreshSupport`.
- **Inlay hints** (B4b; B4_DESIGN.md section 6). `inlayHintProvider` is
  advertised only when the client sends `textDocument.inlayHint` and
  `initializationOptions.inlayHints` is not `false` (off switches: the
  client's own toggle, which stops the requests, and that option, which
  leaves the capability out). Only parameter hints (`kind` 2, label
  `name:`, `paddingRight`), for the `CIX_CALL` events spelled in the file's
  own text (not in a `#define` body, not from an expansion, not in a system
  header). Parameter names come from the index (definition, then
  declarations, then a typedef), and from the text only for declarations in
  system headers (leading underscores stripped). The arguments are re-lexed
  from the snapshot's text. No hint when the argument, outer parentheses and
  leading underscores stripped, is the parameter's name; when a
  `/*name*/` or `/*name=*/` comment precedes it; for a one-parameter setter
  (`setLimit(limit)`); for unnamed parameters, `...` and its arguments, and
  functions without a prototype. Only hints inside the request range are
  returned. Policy `CP_VIEW` (a carried index answers at once and the
  server asks for a refresh when the check publishes). The same lexer
  rules as signature help; both read parameters through `csig_params`
  (src/c/csymidx.c).
- **workspace/symbol** (B4c; B4_DESIGN.md section 7). `workspaceSymbolProvider`
  is always advertised. The answer is `SymbolInformation[]` (`name`, `kind`,
  `location`, `containerName`: the record or enum of a field or enumerator,
  else `""`), at most 100. It has its own handler: no wait, no check is
  started, and it reads whatever index each unit has now. Partial by design
  until B5's project index:
  - only units with an open document are searched (a closed document's unit
    drops out), and a header counts when a searched unit includes it, listed
    once however many units do;
  - a unit still checking contributes its last or carried index; one with a
    snapshot but no index yet contributes its macros only; one with no
    snapshot yet contributes nothing;
  - system headers are left out (user macros and the document symbols'
    selection of C declarations are listed, one entry per decl per unit, at
    its definition if any).

  Matching is a port of clangd 20.1.8's, as probed: case-insensitive, the
  query's characters in order, the first at a segment start (the start, after
  a non-alphanumeric, a lower-to-upper change, the capital before a lower-case
  letter after a capital; a digit is lower case, so a letter-digit change is
  not one), each other right after the previous one or at the next segment
  start after it (no word is skipped: `lea` matches `leaf`, `CALL_LEAF`,
  `__LEAF`; `lf` matches none of them nor `luaC_fix`; `u8` matches nothing in
  `utf8_decode`). Symbols of open documents use that match alone; symbols of
  included headers must also pass clangd's trigram (Dex) test, so a few
  header names match fewer queries. Known residual: queries with two
  underscores on a few header names (B4_DESIGN 19).
  Ranking: exact, exact ignoring case, prefix, other matches by fewer
  jumps (non-adjacent matches, the first free); then C declarations before macros, shorter, name, path, offset.
  Kinds: `SymbolKind` of the document symbols (macros Function or Constant;
  clangd answers 15 for macros, 5 for structs and 10 for enumerators).
- **Code actions** (C4; C_DESIGN.md section 7). `codeActionProvider`
  (`quickfix`, `source.fixAll.cereal`, no resolve) is advertised only when
  the client sends `textDocument.codeAction.codeActionLiteralSupport`; cereal
  has no commands.
  - *Sources of fix-its:* the macro phase's diagnostics of the snapshot, and
    the check's, kept on `CDiag` (`CFix`: path, offsets, text, applicability)
    only if every file they edit has the snapshot's text. `cdiags_carry`
    moves them through an edit like the range; an edit that meets any of a
    diagnostic's fixes drops the whole group and keeps the diagnostic.
  - *Request policy `CP_SNAP`:* waits (1.5 s) only for the snapshot of the
    newest edit, never for its check; a snapshot still older than the
    document answers `[]`.
  - *One action per diagnostic* that meets the request range and has a fix
    group: `quickfix`, `isPreferred` only if every edit is SAFE, MAYBE and
    INFO groups offered without it. The title is `Insert 'X'`, `Replace
    with 'X'`, `Remove 'X'`, `Add #include <h>` or `Apply fix-it`. The
    diagnostic in the action is the client's own from `context.diagnostics`
    when it has the same range and message, else cereal's.
  - *`source.fixAll.cereal`:* offered when the request asks for it, or when
    it has no `only` and the document has a SAFE group. Its edit is what
    `cereal fix` would do to the document (`fix_unit_edits`: the same merge,
    `--only`-free, SAFE only, same three-phase gate with bisection; passes
    until nothing is left to fix or `FIX_MAX_PASSES`, 8, as the CLI), over
    the buffers of the snapshot, run on the builder thread and kept with the
    snapshot. After more than one pass the edit is one replacement from the
    first changed byte to the last. If the run is fatal or the document
    changed meanwhile, the fix-all action is left out. The first request for
    a snapshot is answered by the builder thread (`DeferredReq`) when it has
    done the work, so the protocol thread keeps serving other requests
    meanwhile (as for a C rename's checks); responses can come out of
    order.
  - *Edits:* `documentChanges` with the overlay's document version when the
    client declared `workspace.workspaceEdit.documentChanges`, else
    `changes` as rename; positions in the negotiated encoding.
  - *Test hook:* `cereal/holdChecks` takes `"builds": true` to hold the
    builder before each build as well, so that a snapshot goes stale. A held
    fix-all or C rename (any deferred request) waits at its start, like a
    check.
- **C names:** definition, declaration, type definition, references,
  document highlight, hover, rename, document symbols, signature help,
  semantic tokens and completion (above). Every request that reads the C
  index has a wait policy (a column of the request table in server.c; D1
  above).

## Tests

`tests/lsp_session.py` runs scripted sessions (`tests/lsp/*.json`) and
compares transcripts with golden files. Scripts can create files (a
compilation database with absolute paths), open workspace files, wait
for notifications (`wait`: the latest one; `waitall`: every one not yet
recorded, e.g. both phases' publications of one edit), and use `waitIdle` barriers so asynchronous builds
stay deterministic. An `{"env": {...}}` step sets the server's
environment (`tests/lsp/csym_nocheck` uses it to turn the check off).
`tests/lsp/csym` covers C definition and declaration for every kind,
shadowing, macro-vs-C precedence, D1 and D2; `tests/lsp/csym_refs`
references and highlight (function, redeclarations, shadowing, parameter
with a macro-argument use, typedef, enumerator, a field name in two
structs, label, tags against an ordinary name, a redeclaration chain with
a `#define` body use left out, `includeDeclaration` false, macros
unchanged, D1); `tests/lsp/csym_hover` hover (a variadic prototype,
static const, extern volatile and const-pointer variables, parameters one
of them used in a macro argument, a typedef, an enumerator and its enum,
fields with a bit-field and a designator, struct, union and a 20-member
struct cut after 16, a label, shadowing, macro-vs-C with "(also a macro
name)", D1: an edit then at once a hover, answered from the carried index); `tests/lsp/csym_rename`
rename (a parameter used in a macro argument; refusals for a `#define`
body use, a header declaration, a keyword, a non-identifier, a new name
in `#if 0`, a capture; a rename right after an edit, D1; the macro path
and null on a keyword); `tests/query/csym.cmd` the same merge from the
command line, and `tests/query/rename.cmd` 36 renames (accepted
for every kind and namespace, refused by each blocker and by the second
check). `tests/lsp/csym_b3` covers B3: type definition (typedefs, an
enum, a struct, a header struct through a parameter; none for `int` or a
function-pointer field), document symbols (nesting, an anonymous struct
under its typedef, enumerators of a nameless enum standing alone),
signature help (a function-pointer field, a typedef function pointer from
a header, a variadic function, a macro, a static function), semantic
tokens (whole file and a range) and completion (inside a function and in
another), and, for a client without `refreshSupport`, a token request
right after an edit (it waits for the check; no refresh is sent, the
`absent` step of lsp_session.py); `tests/query/b3.cmd` `cereal query type`
and `visible` on `tests/symidx/b3.c`. `tests/lsp/ccalls` covers C call hierarchy
(prepare on a function, a declaration, a macro, a file-level variable, a
local and a blank place; incoming and outgoing with grouping and
`fromRanges`; the declarator split in `int a = f(), b = g();`; a call through a macro body; items
in an unopened header, function and macro, carrying `data` {name, doc};
an item with its data stripped; stale items after an edit above);
`tests/query/ccalls.cmd` the same from the command line (system, implicit
and declaration-only callees, recursion, `_Static_assert`); `tests/symidx/calls.c`
the dump with ` call` and `xcall` lines and `tests/symidx/carry/calls_*` their carry. `tests/lsp/carry_cidx` (checks held
with `cereal/holdChecks`; the client declares `refreshSupport`): after an
edit that makes a variable `long` and types a declaration and a call,
hover and definition below it (moved), hover above it (no marker), hover
on the edited variable (the marker), completion (`isIncomplete`, without
the edited and typed locals), signature help on the typed call, semantic
tokens, document symbols (ranges moved), hover on the typed name and
prepareRename (each waits out 1.5 s: nothing, "retry"), a second edit
while held (a carry from a carried index), then the release: the refresh
request is recorded, a token delta colors the damage, and hover and
completion are fresh. `tests/lsp/carry_tie` (checks held): a `#define foo`
typed above `int foo;` makes a macro token and a carried C token start at
one place; the semantic tokens show the macro's (fails without the tie
rule). `fault_check` also shows the carried index going with
a failed check. The `wait` step records server requests without params.
The code action sessions (workspace `tests/lsp/fixit/`): `fixit_actions`
(a lint and a check fix, SAFE and not, an include fix, the client's own
diagnostic echoed, `only` filters, the fix-all with a `fixall` step that
applies it and compares the text with `cereal fix` on a copy, a MAYBE fix,
a fix the gate refuses), `fixit_carry` (checks held: fixes moved by an edit
above them, one dropped with its group by an edit at its edge, the
diagnostic kept; a document that needs two passes), `fixit_stale` (builds
held: `[]`), `fixit_nolit` (no `codeActionProvider` without literal support)
and `fixall_async` (checks held: a fix-all is not answered, a hover and a
quick fix are, the fix-all follows the release; steps `defer`, `await`,
`unanswered`).
`tests/lsp/hints` (B4b): every row of B4_DESIGN 6.5 in one function
(`hints.json`; a first-line range and a second-line range; checks held: a
hint request after an insert at the top is answered at once from the carried
index, hints moved down a line; after the release one refresh of each view
is recorded, and no second pair);
`hints_nocap` (no `textDocument.inlayHint`: no provider, no refresh) and
`hints_off` (`initializationOptions.inlayHints` false: no provider).
`tests/lsp/wsym` (B4c): two units sharing a header (the shared symbols
once), queries `""`, exact, prefix, segment, acronym, a skipped word (no
match), a field and an enumerator with their `containerName`, system
symbols absent, a closed document, a unit whose first check is held (its
macros only, answered while it is held) and the 100 limit (150
declarations). `tests/lsp/wsdigits` (Round 221): digit-heavy names in an open
file and a header (`utf8_decode`, `IPv4addr`, `sha256sum`, `a2b`, `aB3cD`),
queries probed against clangd; a digit starts no segment.
`bench/tools/clangd_diff.sh` (not in run.sh: needs clangd) compares hints,
call hierarchy and workspace/symbol (13 queries) with clangd on `bench/clangd_diff/` and three corpus
files; every difference is either a named intended one or `UNEXPECTED`.
`tests/lsp_stress.py [--calls] [--hints] [--symbols] BIN FILE [EDITS] [SEED]` (not a
golden) sends edits each
followed at once by token, completion and hover requests (`--calls` adds
prepareCallHierarchy with incoming and outgoing calls on hand-built items,
`--hints` an inlayHint request over a random range, `--symbols` a
workspace/symbol request with a short query); run on zstd.c
under TSan (also on a smaller file, where more checks publish between
edits) and ASan with the sanitizer logs empty. The sessions are also
run under ThreadSanitizer and AddressSanitizer/UBSan (set `LSP_STDERR` to
collect reports).

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

Server memory (HISTORY Rounds 201 and 202): after edits of a dense 4 MiB
file the server holds about 300 MB resident (zstd.c: 48 MB), most of it
free memory glibc keeps. Starting it with `MALLOC_TRIM_THRESHOLD_=0`
cuts that by 29 to 62% but makes each check on such files 10 to 20%
slower, so it is not the default; no other threshold avoids the slowdown.

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
