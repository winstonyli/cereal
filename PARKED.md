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
| Listing invocation sites of macros whose body uses a function (Q7) | Call hierarchy already lists them; references do not. Plan: docs/MACRO_SITES_PLAN.md. Spikes S1 to S3 done 2026-10-17: functions are a third of the missing sites; design B (all kinds, an event at each invocation) costs 2.9 to 5.1% of record bytes and needs no format bump; decided B. Rename refuses (R7). | built 2026-10-17 (local); D1 decided no (body token not listed) |
| A macro that names a function but is never invoked (R4/R7 gap) | No record event exists; needs the macro-body identifier spellings in the record (the macro rename plan's per-unit macro-table check closes it for rename without a format change, docs/MACRO_RENAME_PLAN.md). Documented limitation. | parked |
| A macro-aware rename policy | Parked 2026-10-17 by decision: rename keeps refusing (R4/R7). A draft plan to edit whole-word body tokens of external functions and objects is saved in docs/MACRO_RENAME_PLAN.md (three review rounds, spikes S1-S3 not run). Trigger: a user hits the refusal often enough to matter (libuv: 5 of 441 functions; lua54: about 66 candidates). | parked |
| Declarations formed by a macro in the project index | `cindex_symbol_event` rejects a pasted or synthesised declaration (`PROTO(zm)` making `int zm_fn(void);`), so references and call hierarchy do not list it and R7 cannot see it. A rename is still safe (the gate's re-check changes that unit's declarations and refuses; `tests/proj_test.py` rename, `zm.c`). Needs the declaration kept in the index with its expansion-site spelling. Not covered by Q7 design B (checked 2026-10-17, `csymidx.c:2446`): B adds REF events only, and a symbol event's text must be the declared name, which the expansion site (`PROTO`) does not spell; a fix is a new DECL-at-expansion event class whose flag rename, selection ranges, symbol names and the verifier all honour, designed first. | parked |
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

## Items found by a docs scan (2026-10-17; trigger "asked for" unless stated)
Deferred in prose elsewhere and missing from this list until the scan. "Checked" means the claim was grepped against src on that date; rows without it are the doc's word.
| What | Why parked | Evidence | Status |
|---|---|---|---|
| SARIF output (A5), a CI baseline file (C5), macro to inline/enum/const suggestions (C6) | Out of scope for the first fix-it work; no code. | ROADMAP:20,84; C_DESIGN:1288-1304; checked: no `sarif` in src | parked |
| Phase A incremental: reuse the unchanged prefix, checkpoint, early cutoff; header cells shared across files (the clangd preamble idea) | Waits for a measured need. | PARALLEL:249,381-425 | parked |
| Parallel mode keeps going past `__COUNTER__` and state-changing `_Pragma` (prefix sum, resume at the divergence) | Today the whole TU aborts and reruns sequentially. | PARALLEL:177-179; checked | parked |
| Per-token provenance for gcc's "in definition of macro" notes | verify.sh does not score the notes; parked until a corpus or golden needs them. | STATUS:121; HISTORY:2973 | parked |
| Idle memory trim and a slimmer carried index | RSS -39 to -75% but 5 to 15% slower. | ROADMAP:142; HISTORY:3794,3830,4786; checked: no trim in src beyond cache eviction | parked |
| `-m32` target selection; `-imacros` in check and replay; a `deps.sh` compile-commands mode | Host predefined macros and headers are used; the flag is parsed (`compdb.c:252`, `deps.c`) but not applied by check and replay. | HISTORY:3013,4014; A4_DESIGN:310 | parked |
| Reuse the cell build's tokens to skip the second preprocessor pass on large files | Saves under half of a phase that is already small. | ROADMAP:105; HISTORY:3235 | parked |
| A `(file, hash)` fold cache | Not measured as needed. | HISTORY:5133; checked: no `fold_cache` in src | parked |
| Front-end gaps: `vector_size` error once per declaration (gcc: per declarator); `format(printf, f, 2)` undeclared-name message; pr56355-1 and pr83844 (fold-const); dump-ada-spec-14 (needs gcc's Ada dumper) | Low value; reasons in HISTORY Rounds 172 to 175 and 184. | STATUS:107-115 | parked |
| Jump into a VLA scope diagnostic; attribute argument counts | Not implemented. | TYPES:214 | parked |
| `-Wmultistatement-macros` false positive with a guard inside a macro whose body is a nested macro; a posix-poll.c unused-parameter warning is omitted | Corner of the warning's macro handling. | HISTORY:2723,2743 | parked |
| Lexer: a bidi control left open in code is not closed by a later string in another chunk; gcc's follow-up semantic errors for a call whose arguments have a syntax error | Needs deferred closer events and a serial prefix-stack pass; the call is not built. | HISTORY:524-530 | parked |
| Per-unit resolved include edges; solver-guided overlay draws; `EMPTY` for an empty `#if X` (stays OPAQUE) | The edges were parked to S1c; the draws need S1d's solver; `EMPTY` is low value. | S1_PLAN:104,140,144; HISTORY:4920 | parked |
| Config language axes: the extension set and C94's `__STDC_VERSION__` | Round 261 built `std` and `mode` only. | CONFIG_DESIGN:312 | parked |
| Treat a PARTIAL virtual unit as dropped (names not merged, budget passed to the next sidecar line) | A proposal that was reverted, not built. | HISTORY:5253 | parked |
| Lazy body parsing (skip bodies by brace matching, parse on demand) | Compatible with P5; no need shown. | PARSER:25; checked: no skip-body code in src/c | parked |
| Per-project size cap for the cache (B5 6.4) | Only the global `cacheMaxMB` eviction exists. | B5_DESIGN:1941; `cacheMaxMB`, src/lsp/project.c:106 | parked |
| Directory-prefix interning of manifest FILS paths | An estimate of 20.6% of the cache; not implemented. | B5_DESIGN:662-668 | parked |
| A fuzzer on random edits for the carried index (reuse the cells' differential fuzzer) | No consumer asked; `tests/fuzz_cells.py` exists to copy from. | B3_DESIGN:815 | parked |
| A 64-file `TextCache` for project queries | Files are read once per query; p95 is 3 ms without it. Overlaps the stat-validated hash cache row above. | HISTORY:4675 | parked |
| Share only decls and strings so the old event arrays are freed after an edit (`cindex_touched` reads `core->by_decl`) | A cleanup, not a memory fix. | HISTORY:3794,3830; `cindex_touched`, src/c/csymidx.c:2665 | parked |
| A side table to merge decl and scope fields exactly | Saves about 4 bytes per decl. | B3_DESIGN:455 | decided against |
| Move the function-definition code out of `cdecl.c` into its own file | About 28 imports for 13% of the file. | HISTORY:2776,2795 | decided against |
| `_Generic`/`typeof` of a bit-field expression: gcc gives the narrow bit-field type (lvalues, assignment, `++`, `+=` values; `+` stays narrow at 33+ bits); cereal gives the declared type | Probed 2026-10-18 (docs/TAST_DESIGN.md 3.2): only visible through `_Generic`, `typeof` and `__builtin_types_compatible_p` on bit-fields. | TAST_DESIGN 3.2; `cexpr.c:6745` | parked |

## TAST generalizations (docs/TAST_DESIGN.md section 13; trigger "asked for" unless stated)
| What | Why parked | Evidence | Status |
|---|---|---|---|
| One annotation log from the checker's visit (BIND, CONV, INIT records through a single `ann()` API) replacing `sym[i]`, the initializer record and the optional recomputable records | Each record is one store at a decision point today; the log pays only with a third recorded item. Trigger: a third visit-time record is needed. | TAST_DESIGN 13.3 | parked |
| Run the conversion warnings over the elaborated tree (`-Wconversion` as "for each cast node, is it unsafe"), deleting the fused type-and-warning logic | Large and risky; follows once conversions are explicit nodes (T1 onward). Trigger: T3 done and a conversion-warning change is wanted. The gcc goldens are the safety net. | TAST_DESIGN 13.4; `cconv.c` | parked |

## TAST T2 leftovers

| What | Why parked / trigger | Evidence | Status |
|---|---|---|---|
| Pointer-qualification changes elaborate as a `cast` node | Harmless, but noisy against gcc's tree; the consumer may want a distinct `qual` op or none. Trigger: the first back end or the conversion-warning rewrite. | `tests/tast/ptr.expected` `msg` | parked |
| gcc differential compares cast multisets only, not pointer arithmetic or indirections | Coarse oracle by decision; extend if a pointer bug escapes. Trigger: a missed bug. | `tests/tast_gcc.py` | parked |

## TAST T3 leftovers

| What | Why parked / trigger | Evidence | Status |
|---|---|---|---|
| Real K&R definitions (`int kr(a) int a; {..}`) are unsupported | No corpus unit needs them. Trigger: a corpus file uses one. | `tests/tast/unsup.c` `kr` | parked |
| Calls through an implicit declaration (`sym == SYM_NONE`, e.g. libuv `setproctitle`) are unsupported | One libuv unit; an implicit declaration is C89 only and the checker already diagnoses it. Decided 2026-10-18: not worth a rule. Revisit if a corpus has many. | `docs/TAST_T3_PLAN.md` "Verify list" | decided against |
| `(long)&x + 8` at file scope is accepted by gcc but the checker reports "initializer element is not constant" | A checker divergence found by the T3b test; not a TAST matter. Trigger: asked for. | `tests/tast/data.c` (removed line); gcc-13 accepts | parked |

## TAST T4 leftovers

| What | Why parked / trigger | Evidence | Status |
|---|---|---|---|
| Variable length arrays (`int a[n]`, evaluated `sizeof`) are unsupported ("array type") | No corpus unit has one as a reason (lua54, libuv: none left). Needs a runtime-sized `local` and a size expression. Trigger: a corpus unit uses one. | tested 2026-10-18 with `int a[n]` | parked |
| T4b types (`_Complex`, `__int128`, `_FloatN`, decimal) are unsupported, each with its own reason | No corpus unit has one as a first reason (lua54, libuv: none left after T4a). Trigger: a corpus unit whose reason is one of them. | `tests/tast/unsup.c` `dm`, `fl`; run `2026-10-18-tast-t4a-coverage` | parked |
| Non-x87 `long double` formats (`LD_IEEE128`, `LD_IBM128`) and a big-endian host or target make the unit unsupported | `fbytes` copies the host's x87 image. Trigger: a back end for such a target. | `src/c/tast.c` `fset` | parked |
| A floating constant converted to another type stays a `cast` of an `fconst` (`float f = 2.5;` is `cast (fconst double) float`) | Folding it needs a second constant folder; the oracle and a back end read the cast. Trigger: a consumer that wants only leaves. | `tests/tast/fdata.c` `f5` | decided against |
| The data oracle rounds a `long double` cast to `float` through a Python double (two roundings) | Only a tie could differ; no case has one. Trigger: a mismatch on such a cast. | `tests/tast_data.py` `enc_float` | parked |

## TAST: units left after variadic/`va_*` (2026-10-18)

lua54 has 2 distinct unsupported units and libuv 6; each needs its own mechanism.

| What | Why parked / trigger | Evidence | Status |
|---|---|---|---|
| Data oracle skips objects holding a label address (`labaddr`) | A label address is a `.text` relocation with an offset the pass cannot know; the oracle skips the object instead of comparing positions (`docs/TAST_T6_PLAN.md` section 4). Trigger: a unit whose other data in the same object needs checking. | lua54 `lvm.c` `disptab` (2 objects skipped, 2026-10-18) | parked |
| `asm` outside the supported subset | Operands work for constraints a/b(cpuid)/c/d/r/+ and templates of cmp, test, mov, add, sub, and, or, xor, imul, cmovCC (`asm_emit`, `src/c/cgx86.c`); anything else is unsupported. Trigger: a corpus unit needs another instruction or constraint. | `gen_asm_ops` | parked |

## TAST T8 leftovers (docs/TAST_T8_PLAN.md sections 7 and 12; verified 2026-10-18)

| What | Why parked / trigger | Evidence | Status |
|---|---|---|---|
| Execution oracle for floating point, aggregate copies, bit-fields, type punning, pointers passed between calls, indirect calls | The interpreter models one cell per object and byte offset; these need a byte-level memory and an ABI model. Trigger: a lowering bug in these areas that the structural verifier and corpus gate missed. | `tests/cfg_exec.py` header; aggregate-typed `?:` lowers with 0 findings (`pick` in a scratch file) but its value is not compared | parked |
| Optimization, SSA, ABI classification of calls, lowering of initializers and `local` into stores, `setjmp`/`longjmp` edges | Out of scope of T8 by decision: `reg` is a mutable variable; a later pass builds SSA. Trigger: the back end (Track D) starts. | `docs/TAST_T8_PLAN.md` section 7 | parked |

## x86-64 back end leftovers (src/c/cgx86.c, mir_elf.c; verified 2026-10-19)

| What | Why parked / trigger | Evidence | Status |
|---|---|---|---|
| Aggregate `const` values (`cond` and `comma` work, probed 2026-10-19); bit-field elements in a local initializer list; register-class aggregates whose last eightbyte is 3, 5, 6 or 7 bytes; `long double`/complex members | The "value is an address" rule (`docs/TAST_T9_PLAN.md`) covers load, store, call and local init; these forms have no address yet. Trigger: a corpus unit needs them. | none in the corpus (2026-10-19) | parked |
| Built-ins beyond `expect`, `huge_val`/`inf`, the libc-named string and memory ones, the bit counts and the atomics (every `__atomic_*` and `__sync_*` the front end resolves) | `__builtin_assume` and the clang-only `__builtin_rotateleft32/64` (gcc has neither; the corpus uses them outside emitted code; no corpus use of `__builtin_*` math, 2026-10-19). Trigger: a corpus function reports `built-in NAME`. | `gen_intrin` in `src/c/cgx86.c` | parked |
| `__builtin_alloca`, `trap`, `frame_address`, `object_size`, `case` ranges, nested functions and `__label__` are unsupported; overflow built-ins on 64-bit values whose exact result needs more than 64 bits to test (unsigned 64-bit with signed, unsigned into signed, signed 64-bit into unsigned 64-bit) | docs/C_STANDARD.md survey (2026-10-19; the overflow built-ins, stdckdint.h and `__builtin_stdc_*` were built 2026-10-20). Trigger: a corpus function reports one of these. | `tools/c_survey.py`, `gen_ovf` in `src/c/cgx86.c` | parked |
| `-std=c89/c90/gnu89` and `-std=c2x/c23/gnu23` are refused (fatal); C23 syntax (`nullptr`, `constexpr`, `typeof`, `bool`/`true`/`false`, `[[attributes]]` in all positions, `_BitInt`, `#embed`, `'` separators, `u8''`, one-argument `va_start`) fails the front end | docs/C_STANDARD.md: 13 of 114 probes, all C23. Needs its own plan (lexer, parser, keyword set per standard). Trigger: a corpus builds with `-std=c23` or `-std=c89`. | `src/c` option table (A3) | parked |
| Initializers the data evaluator rejects: `long double` and compound literals (comparisons, division and `?:` in constants work, probed 2026-10-19). Variable attributes `section`, `weak`, `tls` are silently ignored; a reference to an `alias`/`ifunc` symbol is reported unsupported (the alias itself is not emitted) | The evaluator folds integer, float and address arithmetic only (tests/tast_data.py has the same set). Trigger: a corpus object reports `# x86: NAME unsupported`, or a corpus object carries one of those attributes. | `src/c/cgdata.c` `ev`; no attribute is read there | parked |
| Read-only objects holding addresses go to `.data`, not `.data.rel.ro`; tentative definitions are never `common` | Equal at run time; matters only for a shared-library link or `-fno-common` comparisons. Trigger: byte comparison of object files with gas for data. | `src/c/cgdata.c` `x86_data_node` | parked |
| `long double` | x87 for `long double`. Trigger: a corpus unit uses it. | `tinfo` in `src/c/cgx86.c` | parked |
| Corpus gaps left after zstd compiled and round-tripped (2026-10-19; `tests/zstd_e2e.sh`) | `aggregate in a register or temporary` (`gz_decomp`, zstd zlibWrapper). Trigger: a full-zstd end-to-end target. | `why` strings in `src/c/tast.c`; `cgx86.c` `unsup` | parked |
| `__builtin_popcount*` and `parity*` as the `popcnt` instruction (2026-10-19, by decision) | They call libgcc's `__popcountdi2`/`__paritydi2` as gcc does without `-mpopcnt`; the instruction raises the CPU baseline (no `popcnt` before 2008). Trigger: bit counting in a hot corpus loop. | `BIC_RT` in `src/c/cintrin.h`; `BIC_RT` case in `gen_intrin` | parked |
| The `__builtin_stdc_*` functions (`<stdbit.h>`) as shorter sequences (2026-10-20, by decision) | `gen_stdc` patches the zero case of `bsr`/`bsf` with a `cmov` and calls libgcc's `__popcountdi2` for `count_ones`/`count_zeros`, as the existing `clz`/`ctz`/`popcount` rows do. Left: `lzcnt`/`tzcnt`/`popcnt` forms (they raise the CPU baseline, see the `popcnt` row above) and an open-coded popcount instead of the call. Correct against gcc on every width (`tests/asm_exec/stdc.c`). Trigger: bit scanning in a hot corpus loop. | `gen_stdc` in `src/c/cgx86.c` | parked |
| The value-computing branches of `e_call` as rows (2026-10-20, by decision to hold) | `expect`, `constant_p`, `__builtin_LINE` and `loc_builtin` keep their result types and folds in `e_call`; the six that call a dedicated function (`choose_expr`, `call_with_static_chain`, `shuffle`, `shufflevector`, `tgmath`, `complex`) are one-line dispatches. A row could give the result type, but the folds stay per-name code, so a `fold` field would move three special cases into a switch without deleting any. Still open: it becomes worth doing if a fold rule is shared (a constant argument yielding a constant) by more built-ins. Trigger: a fourth built-in with the same fold shape. | `e_call` in `src/c/ccall.c` | held |
| Real `%xmm` vector code (2026-10-19, by decision) | a vector is a memory aggregate and every operation is lowered lane by lane to scalar code; vector arithmetic never uses `paddd`/`addps`. Correct and simple, slow for SIMD-heavy code. Trigger: SIMD-heavy corpus code where speed matters. | `vmap` in `src/c/tast_vec.c`; `vec_sig_ok` in `src/c/cgx86.c` | parked |
| Vector types, left after the SSE2 integer core (2026-10-19; `tests/asm_exec/vec.c`, zstd with intrinsics) | (a) 8- and 16-byte vectors now travel as SysV does (class `s`, and `v` for a whole `%xmm`, `M_MOVUPS`; `tests/asm_exec/vabi.c` against gcc, both ways); left: a vector that does not fit the registers (9th `%xmm` argument; refused, the stack slot needs 16-byte alignment), vectors wider than 16 bytes (memory class between our own functions only, a non-static function with one is refused by `vec_sig_ok`), `va_arg` of a vector; (b) done 2026-10-19: vector `++`/`--`, and a `__builtin_shuffle` mask that is a constant (read at compile time); decided against: vector `?:`, `!`, `&&`, `||` and scalar-to-vector casts are C++ only (gcc 15 rejects them in C, as `cexpr.c` does); `__builtin_shufflevector` index -1 reads lane 0, gcc leaves it undefined;|\|`, `++`/`--`, scalar-to-vector casts, a `__builtin_shuffle` mask that is a constant (it always indexes through the vector's address; `__builtin_shufflevector` index -1 reads lane 0, gcc leaves it undefined); (c) x86 built-ins outside SSE2 (done 2026-10-19: integer, `ps`/`pd`/`ss`/`sd` arithmetic, compares, shuffles, truncating converts, `sqrt` as the instruction, converts that round as MXCSR says (`cvtss2si`, `cvtps2dq`, ...), fences/`pause`/`clflush` as MIR ops, `movnt*` as plain stores); left (measured 2026-10-19 by taking the address of every `_mm_*` function of `xmmintrin.h`/`emmintrin.h`, 0 of 354 fail; `rcp`/`rsqrt` are the instructions, so their results are the CPU's approximations, equal to gcc's only on the same CPU family): SSE3 and later, AVX (no corpus unit uses any); done the same day: `loadl`/`loadh`/`storel`/`storeh` (`_pi`), `getcsr`/`setcsr` (the rounding converts follow MXCSR, tested in all four modes), 8-byte `__m64` functions (MMX forms of the SSE builtins, `vec_init`, vector/integer bit casts), `maskmovdqu`, `movntq`; table rows in `src/c/cbuiltin_x86.h`; (d) static data of a system header is not emitted when referenced from an emitted system function (only functions are deferred). Trigger: (a) code that passes more than eight vectors or uses `__m256`; (b), (c) a corpus unit that needs one; (d) a system header with static tables. | `vec_sig_ok` in `src/c/cgx86.c`; `ia32_call` in `src/c/tast_x86.c`; `settle_pending` in `src/c/cgdata.c` | parked |
| Code speed: Lua is 3.0x slower than gcc -O0 and 7.3x slower than -O2 (2.49 s vs 0.84 s vs 0.34 s, run `2026-10-19-lua-speed`) | Every value goes through %rax and push/pop spills; no register allocation or peepholes. Candidates, cheapest first: peepholes (push/pop pairs, redundant reloads), then a register allocator over the T8 CFG. Parked by decision: correctness and coverage first. Trigger: speed matters for a use, or the corpus work is done. | `docs/TAST_DESIGN.md` spike row; samply profile of the Lua build not yet taken | parked |
| The oracle's random float arguments avoid out-of-range conversions | A float-to-int cast out of range is undefined and differs between 32- and 64-bit `cvtt`; the tests keep values small. Not a back end bug. | `tests/asm_exec.py` `value` | decided against |
