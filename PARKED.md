# Parked work

Format: what; why parked; trigger; evidence; status. This is the one list: the design docs' former "Parked" sections now point here. Migrated 2026-10-17; where an item had no stated trigger, the trigger is "asked for", and evidence is the design section it came from (line numbers not re-verified in the migration).

## Config and presence conditions (docs/CONFIG_DESIGN.md)

### Live settings reload (`workspace/didChangeConfiguration` for `cereal.index.*`)
- **Why:** no index setting reloads today. `P.set` has 33 uses across server and scheduler threads, the pool is sized at startup, the store opens once. A reload must run as a scheduler-thread event, swap only live fields (`configs`, `maxOverlays`, `renameMaxTUs`, `quietMs`, limits), refuse the rest (`enable`, `threads`, `cacheDir`, `paths`), and for `configs` call `pstate_set_virtual` and re-stale real units without a sidecar. A restart applies any setting.
- **Trigger:** a client asks. **Evidence:** `load_settings`, src/lsp/project.c; CONFIG_DESIGN 11. **Status:** parked (also B4 15 Q5 option 3).

### Adaptive `cereal.index.configs`
- **Why:** on by default, off once the measured index time passes a budget; the cost is known only after a first index, so first and later runs would differ. Measured ~5.5x index time at 0.5 (Round 279).
- **Trigger:** the per-unit cost drops further. **Evidence:** CONFIG_DESIGN 10 item 4, 11; HISTORY Round 279. **Status:** parked.

### Bounding the entries kept per macro in `do_define`
- **Why:** would cut `pc_fold` (37% of the `lj_state.c` profile, `do_define` 21%) but makes the fold inexact.
- **Trigger:** `pc_cover` cost is the bottleneck again and an inexact fold is accepted. **Evidence:** `do_define`, src/analysis/pc.c; HISTORY Round 279. **Status:** held for a user decision.

### Smaller items
- Variational parsing (R5), arithmetic atoms (OPAQUE; rare), `__COUNTER__`/`__FILE__`/`__LINE__` in conditions (OPAQUE): by design. Evidence: CONFIG_DESIGN 11. Status: parked.
- A configuration-space view: S1's result is the data; the view is a client matter. Status: parked.
- Cross-target type checking: needs the target model of CONFIG_DESIGN 7.3. Status: parked.

## Project index and rename (docs/B5_DESIGN.md, section 18)

| What | Why parked | Status |
|---|---|---|
| `didChangeWatchedFiles` | The stat sweep covers the same events (E7); it would only make detection faster. | parked |
| Macro references across TUs | Q1; needs a macro identity. | parked |
| Listing invocation sites of macros whose body uses a function (Q7) | Call hierarchy already lists them; references do not. Plan: docs/MACRO_SITES_PLAN.md. Spikes S1 to S3 done 2026-10-17: functions are a third of the missing sites; design B (all kinds, an event at each invocation) costs 2.9 to 5.1% of record bytes and needs no format bump; decided B. Rename refuses (R7). | built 2026-10-17 (local); open: D1, whether references also list the macro-body token (clangd does not) |
| A macro that names a function but is never invoked (R4/R7 gap) | No record event exists; needs the macro-body identifier spellings in the record (the macro rename plan's per-unit macro-table check closes it for rename without a format change, docs/MACRO_RENAME_PLAN.md). Documented limitation. | parked |
| A macro-aware rename policy | Parked 2026-10-17 by decision: rename keeps refusing (R4/R7). A draft plan to edit whole-word body tokens of external functions and objects is saved in docs/MACRO_RENAME_PLAN.md (three review rounds, spikes S1-S3 not run). Trigger: a user hits the refusal often enough to matter (libuv: 5 of 441 functions; lua54: about 66 candidates). | parked |
| Declarations formed by a macro in the project index | `cindex_symbol_event` rejects a pasted or synthesised declaration (`PROTO(zm)` making `int zm_fn(void);`), so references and call hierarchy do not list it and R7 cannot see it. A rename is still safe (the gate's re-check changes that unit's declarations and refuses; `tests/proj_test.py` rename, `zm.c`). Needs the declaration kept in the index with its expansion-site spelling. | parked |
| Renaming through inactive `#if` groups (A3.9) | Refuses a use in a group the unit does not compile (user decision, Round 231). Editing needs per-branch preprocessing or a textual mode verified under the other branches. S4 (CONFIG_DESIGN 8) adds precondition lists and may allow it via `REALIZED` units. | parked; see S4 |
| Shadowing detection for new headers | Needs every probed directory recorded (7.2). | parked |
| Cross-TU type mismatch diagnostics through csum `type_dig` | Not requested; csum needs `CheckOptions.summaries` (+4% check). | parked |
| A weak flag in `CIdxDecl` to order definitions | Only cosmetic (12.4). | parked |
| An n-gram index for workspace/symbol | Trigger: the B5b stop rule fires. | parked |
| Indexing unsaved buffers of open files into the store | Open units already overlay. | parked |
| Remote or shared caches between machines | Host-endian keys (5.4). | parked |
| `$/cancelRequest` for requests answered at once | Nothing to cancel: the protocol thread answers each before reading the next message. Deferred renames and queued code actions are cancellable (Round 234); a running fix-all is not. | parked |
| Type hierarchy | D7. | parked |
| A stat-validated hash cache for `PqFiles` | A query reads and hashes each file it places entries in; `PQ_MAX_ENTRIES` bounds the files, the page cache makes a repeat cheap. Needs the record's own stat. Trigger: references p95 above its limit (3.3 ms measured). (Round 232 F8) | parked |
| Releasing a file's text after its last edit is written | The peak is the sum of the files a result lies in, bounded by the entry cap. (Round 232 F8) | parked |

## Call hierarchy and hints (docs/B4_DESIGN.md, section 15)
- GNU nested functions: their calls are attributed to the outer function.
- Hints for calls whose callee is in a `#define` body (H1).
- Type and chain hints, and the type hierarchy (D1).
- Cross-TU callers and callees, and the workspace beyond open units (B5 covers part).
- Call hierarchy items for sites with no caller (`_Static_assert` at file level).
- Trigger for all: asked for. Status: parked.

## Completion and scopes (docs/B3_DESIGN.md, section 9)
- Member completion after `.`/`->`: the data exists, but it is a different request context (the expression's type, not the scope).
- Tag completion after `struct`/`union`/`enum`, label completion after `goto`, keywords: the cursor context, not scope data.
- Signature help through an expression callee (`(*fp)(`, `a[i](`).
- Blocks written inside a macro argument (`WITH_LOCK(m, { int x; ... })`): scope extents are presentation points, so such a block is `[invocation, +1)` and completion inside it does not offer `x`. Fix: spellings where both braces are spelled outside `#define` bodies (~15 lines). Not needed for the common `FOREACH(..) {` form.
- Trigger for all: asked for. Status: parked.

## Fix-its (docs/C_DESIGN.md, 12.3 and 4.2)
| What | Why parked |
|---|---|
| Caret-block fix-it line | The caret block is not gcc-identical yet |
| Unused-variable deletion | gcc has none; removing a declaration can remove side effects, and a gate cannot see intent |
| Macro trailing `;` and do-while wrap | Rewrites macro bodies; belongs with C6's macro edits |
| Missing header guard | Must invent a name; style-dependent |
| Multi-pass `--diff` | Needs simulated passes through overlays; one pass is enough to review |
| Fix-its on `-D` (virtual) or system files | Decided against: no source to edit |

## Guard-of-header preconditions (docs/CONFIG_DESIGN.md section 6, S4)
- **What:** offering `defined(GUARD)` ("skip that header") as a precondition when the header includes a system header.
- **Why parked:** the fold replays system headers from the real run, so what a later include of the same header would define when the first is skipped is not modelled; the list was wrong for 14 of libuv's 54 refusals (Round 280). `explain_fixed` never offers it.
- **Trigger:** the fold reads a system header again for a guard-skipped re-include (or `sysbelow` proves the header safe).
- **Evidence:** `explain_fixed`, src/analysis/pc.c; `bench/tools/s4_falsify.py`; HISTORY Round 280.
- **Status:** parked.

## `#if MACRO` lists show two cubes (`X < 1 && X < 0` or `X >= 1 && X >= 0`)
- **What:** a plain `#if FEATURE_X` is split by the atom model into "value below 0" and "value at least 1"; the lists print both (the first reads oddly; sound, since negative values are nonzero). `pc_enable_group` now prefers the nonnegative cube, so actions and overlays are fine.
- **Why parked:** presentation only; merging needs a "nonzero" atom form.
- **Trigger:** the lists are shown to users who find it confusing.
- **Evidence:** tests/lsp/pccfg.expected (2026-10-17).
- **Status:** parked.

## Full port of cereal's settings to the macro-style config header
- **What:** moving the 14 `cereal.index.*` settings (and `.cereal` flags) into the config header's `key = value` form, one config surface.
- **Why parked:** by decision (2026-10-17): the header covers only the analysis-facing choices (`configs`, `maxOverlays`, feature defines). The operational knobs (`threads`, `memoryMB`, cache limits, timeouts, `cacheDir`) say nothing about the analysed program, lists and paths fit `#define` badly, and a port adds a parser and a precedence order across env, client settings, header and `.cereal`, plus a migration. Kept open, not rejected.
- **Trigger:** the header exists and users keep wanting its settings in one place, or live reload (above) lands and needs one file to watch.
- **Evidence:** `load_settings`, src/lsp/project.c:88; docs/LSP.md line 17.
- **Status:** parked.

## Precondition list in the code action, and in the CLI rename refusal (S4)
- **Why parked:** the list is in the project rename refusal and in hover; the code action and `cereal query rename=` (single unit, no fold kept) still refuse without one.
- **Trigger:** asked for, or a client shows refusals only through the code action.
- **Evidence:** `c_rename_explain` (src/c/crename.c) is the one function either would call.
- **Status:** parked.

## Enable-group action beyond the configuration header
- **What:** "Compile this group" for groups whose cheapest overlay is not a header macro or a new header line: `-std`, a macro the project defines in an ordinary header, project-option flags (the header toggles, `-D` of a new macro and `-U` of a header macro, are done).
- **Why parked:** needs an edit target: a `.cereal` flags line (persistent, new line), the compile-database entry (lost on regeneration; not recommended), or a per-document in-memory overlay (no file change; the better first step). The header case covers the settings a user keeps in one place.
- **Trigger:** asked for, or libuv-style option groups in a project without a config header.
- **Evidence:** `pc_enable_edits`, src/analysis/pc.c (refuses any word that is not a `-D`/`-U` of a header macro or of a macro nobody defines); docs/LSP.md, Code actions.
- **Status:** parked.

## Older items from prose (migrated 2026-10-17; trigger "asked for" unless stated)
| What | Why parked | Evidence | Status |
|---|---|---|---|
| `--progress` for `check --compile-commands` | Output already streams in order; worth it only for a database large enough to want it. | A4_DESIGN 4.9 (~line 301) | parked |
| Headers without entries in replay use no nearest-entry guess (the LSP does, `config.c`) | Replay checks only what an entry names. | A4_DESIGN 4.10 | parked |
| `-idirafter`, `--sysroot`, `-ffreestanding` and the other `IGN_SEMANTIC` flags; `g++`-compiled `.c` files; MSVC entries | Linux gcc entries only; noted once per run. | A4_DESIGN 4.10 | parked |
| Enum constant in `#if`: a by-name fallback | The preprocessor sees an identifier, so no event; rename treats the group as a blocker. | B2_DESIGN ~line 445 | parked |
| A final whole-set re-run after the fix phases | Each phase is gated on the accepted set; the next pass and the probe check the final text. Trigger: a case where a combination escapes the last pass. | C_DESIGN ~line 1066 | parked |
| Parser errors first: diagnostic order in a unit with syntax errors | Order stays parser-errors-first; matching gcc's interleaving is not needed yet. | HISTORY ~line 174 | parked |
| Sharing record chunks across projects | Needs reference counting; chunks are per project so whole-project eviction stays correct. | B5_DESIGN ~line 826 | parked |

## S3-manifests: edge PCs and instance trees in the manifest (docs/S3_PLAN.md)
- **What:** the inclusion-instance tree with edge PCs and untaken edges (CONFIG_DESIGN 5.3), split from S3-records.
- **Why parked:** needs a fold of every unit at index time (today only with `cover_max`; `pc_fold` is 37% of an `lj_state.c` profile) and no consumer needs an edge PC.
- **Trigger:** a feature that must compose a stored event's full PC without refolding, and a measured fold cost it can afford.
- **Evidence:** src/c/cidxrec.c:833 (the fold runs only for `cover_max`); docs/S3_PLAN.md section 2.
- **Status:** parked.

## S3-records: Local PC ids in the index records (docs/S3_PLAN.md)
- **What:** sparse `DPC`/`RPC`/`CPC` side arrays plus a `PCT` table in `CREC_FORMAT` 5, giving the events with a non-trivial `#if` condition their Local PC id.
- **Why parked:** S3a (2026-10-17) passes the size rule (at most 3.3% of the store on lua54, libuv and cereal `src`; 10.0% on zstd lib, 16% on its amalgamation) but no consumer is chosen, and nothing is blocked without it. Stored ids give display and pre-filtering only; `sat`, `enable` and `cover` still fold.
- **Trigger:** a chosen consumer from S3_PLAN section 1 (references annotation is the cheapest) on a project large enough that refolding its units is too slow (libuv: 1.5 s for all 38 units, so not libuv-sized projects).
- **Evidence:** docs/S3_PLAN.md section 6; run `2026-10-17-s3a` (docs/runs.md); record writer src/c/cidxrec.c `build_record`.
- **Status:** decided against (2026-10-17), unless a project too large to refold on demand appears; S3a and the fold timing are the evidence (docs/S3_PLAN.md section 6).
