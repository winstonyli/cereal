---
status: draft
---

# Cleanup plan (2026-10-31)

Why now: the evaluator work (trapped-call queue, asm in sweep calls) is committed and the gate is green at `0e56e57`; nothing is in flight (`lazy-blobs` has 0 commits ahead of `main`).

Surveys (read-only, five Sonnet agents, commit `0e56e57`): evaluator and solver (EV), the rest of `src/` (FE), tests, bench and tools (TS), docs and hygiene (DC), architecture (AR). Throughput was not surveyed (not asked for). Baseline: no compiler warnings reported; the gate numbers are in `bench/tools/gate.sh`'s header. Claims are the surveys' unless marked `(checked)`; checked by hand on 2026-10-31: FE1 (each of the ten has only its definition and its declaration), FE2 (`DIAG_FMT` appears only in the declaration), TS5 (four of the six byte-identical pairs re-compared), DC1 (`docs/ROADMAP.md:17` and `:195` still say what the survey quotes), and the front-matter states in DC9/DC10.

Protected, not touched: golden corpora, `tests/check`, soundness tests, HISTORY and `docs/runs.md` (history); the checker folder and the evaluator stay separate (decided against merging, EVAL_DESIGN section 2).

## Order and decisions

Not yet chosen. Recommended order: (1) docs, one pass, no code; (2) pure deletions of dead code and write-only fields, one gate; (3) tests, bench and tools trims; (4) the small shared helpers; (5) the architecture items, each as its own plan, AR2 first. Items with an open design option are held for the answer.

## Items

### Docs (DC, EV doc items)

- **DC1. ROADMAP says there is no code generator.** `docs/ROADMAP.md:17` ("Driver" bullet) and `:195` (Track D, "Nothing in the tree lowers the AST today", back end "8 to 12+ weeks") contradict `src/c/cgx86.c`, `mir_elf.c`, `--emit-obj`, `cereal run`. About 6 lines. (checked)
- **DC2. STATUS "What exists" and README usage omit TAST/CFG, back end, evaluator, `check`.** About 10 lines each plus one ARCHITECTURE paragraph (ARCHITECTURE is preprocessor-centric and lacks the `src/c` modules).
- **DC3. `-std=c2x/c23` still called fatal.** `docs/ROADMAP.md:17`, `:70`, README usage; `src/driver.c:307` accepts them (`docs/C23_PLAN.md` Steps 1-4). 3 edits.
- **DC4. README parity numbers stale and duplicated from STATUS** (634 of 636 against 637 of 638). Keep numbers and the gcc-testsuite recipe in STATUS; README links. About 12 lines.
- **DC5. README commit statistics stale** (266 against 815 commits). Drop or compute at tag time.
- **DC6/DC7/DC8. Q7 (macro invocation sites) is built but described as unbuilt:** `docs/MACRO_SITES_PLAN.md` (`status: draft`, "nothing is built"), `docs/LSP.md` (~591-596, "needs a record format bump"), and the PARKED Q7 row (built; only D1 "decided against" is deferred). Fix the three; cut the PARKED row to the D1 line.
- **DC9/DC10. Front matter that no longer matches the work.** `TAST_T1_PLAN` and `TAST_T8_PLAN` active but finished (checked against their own Progress and sections 11-12) -> `done`; `TAST_DESIGN` draft though built through T9; `PARSER` active with P0..P6 built; `S1_PLAN` and `C23_PLAN` need a read of their remaining steps first. `STOPPED_SEEDS_SPIKE.md` has no front matter and no entry in CLAUDE.md's lists (DC16/EV20): add `status:` and name the exceptions (`C_STANDARD`, `runs.md`, `bench/BASELINE.md`) in CLAUDE.md.
- **DC11/DC12. STATUS has two test counts and duplicates PARKED rows** (Parked front-end gaps; "Not scored by verify.sh"). One home each; link from the other. About 18 lines.
- **DC13. PARKED evidence cites other docs by line number** (`ROADMAP:20,84,142,105`, `STATUS:121`, `HISTORY:...`). Cite headings. About 25 cells. Code-side `file:line` pairs mostly hold (DC14); two are off by one or a few lines (`cexpr.c:6745`, `csymidx.c:2665`).
- **DC15. PARKED rows 239 and 245 are multi-KB cells of results; row 225 has a garbled repeated fragment.** Cut to what/why/trigger/status plus a link to the section holding the results (CONCOLIC_DESIGN, LAZY_BLOBS_DESIGN, STOPPED_SEEDS_SPIKE). Also EV22: row 245 gives the lazy cap as 8192 and as 65536 (code: 65536).
- **EV2/EV4/EV5/EV11/EV21 (comments in code).** `eval.c` header says floats, bit-fields, compound literals, variadics, built-ins, globals are "not yet covered" (all built); three doc comments in `eval_run.c` sit above the wrong function (`lazy_byte`, `hint_put`, `state_variants`); the `lazy_fill` pointer-draw comment disagrees with the code (check LAZY_BLOBS_DESIGN first); `eval_libc.c` header promises a "future shadow"; `tests/eval_lazy.sh` line 2. About 20 lines.
- **EV19. Docs name symbols that no longer exist** (`sweep_ptrs`, `SMAXOB`, `EVAL_SWEEP_LAZY`, `--eval-lazy`, `eval_sweep_all`, ...). All in `done` plans or dated results: fix only the sections that describe current behaviour (EVAL_DESIGN.md, LAZY_BLOBS_DESIGN.md).
- **TS11/TS12. `tests/run.sh` header index and `bench/tools/gate.sh` header/`docs/STATUS.md:76` describe the suites and the gate wrongly** (missing sections 15, 18, 20, 22b/c, 23, 24; thread counts; "runs all of it"). About 8 lines.
- **EV1. `main.c` usage text splices `run` into the middle of the `check` options.** About 6 lines moved.

### Code: dead and write-only (FE, EV)

- **FE1. Ten dead extern functions** (def + declaration only): `cexpr_find_member`, `cfind_semi`, `checker_summary`, `scope_depth`, `type_vla_sized`, `type_param_adjust`, `str_eq_n`, `diag_find_option`, `json_flush`, `pp_presumed_name`. About 85 lines. Doc mentions to touch: `TYPES.md:95`, `HISTORY.md:100` (history, leave), `CHECKER_PLAN.md:171`, `TAST_T2_PLAN.md:213`. (checked)
- **FE2. `DiagFormat` / `DiagContext.format`** and the stale comment (`src/diag.h:76,106,108`). 3 lines. (checked)
- **FE3. Write-only or unread struct fields:** `GDecl.funcdef_ok`, `RecDef.has_neg`, `CCtx.found_mb_unused`, `UnitSummary.priv`, `FdParts.scope_end`, `Target.max_align`, `Spec.has_attrs`, `Spec.alignas_loc`, `IncludeEvent.macro_expanded` (and the local that feeds it), `Skeleton.guard_ifndef_index`, `LineNote.line_start`, `PPath.taken_ns`; **EV3** `Ev.rsh`. Hold `CRecKey.kflags` (may be an on-disk slot: check the record format version first). About 25 lines.
- **FE4. Unused enumerators:** `PR_CAST`, `CSF_KR_PENDING`, `TSK_RESERVED`, `D_IDENT`, `CL_NONE`, `TF_DOLLAR`, `LD_IBM128`. Check no value or table index depends on the position (`PR_CAST`, `D_IDENT` are mid-list; `LD_IBM128` and `long_double` indexing). About 7 lines.
- **FE5. `(void)nacc` / `(void)nfix` counters** (`fixset.c`, `fixmode.c`). About 8 lines.
- **EV13/EV15/EV16/EV14 (small).** `ev_new` sets `v->fuel` that `start_budget` always overrides; redundant first `blk_count` and `v->track = false` in `eval_sweep`; `stream[3]`/`token[3]` use only indices 1 and 2; `Ev.note[112]` could be local; trivial wrappers `mut_solve`, `trap_keep`; `SR_NONE` never compared (grep to confirm); `h_fflush` unused local `f`; `symx.c:nd()` against `sx_at()`. About 25 lines. Re-verify each.
- **EV7. `ev_of` builds `EvOpts` twice with identical code.** About 8 lines.

### Code: duplicated helpers (FE, EV)

- **FE6 is subsumed by AR2** (comparison swap/mirror/is-compare); do it there.
- **FE7. Six copies of the uint32 `qsort` comparator** (`bdd.c`, `cdecl.c`, `cell.c`, `fixmode.c`, `csymidx.c`, and the `name_cmp`/`opt_cmp`/`cmp_keys` family). One `cmp_u32` in `common.h`. About 20 lines.
- **FE8. Four to seven copies of the monotonic-clock helper** (`main.c` twice, `par.c`, `lsp/server.c`, plus direct `clock_gettime` in `analysis.c`, `project.c`, `cidxrec.c`). One `now_sec` in `common.c`. About 18 lines.
- **FE9. FNV-1a copies** (`fixmode.c`, `fixtu.c`, `cdigest.c` identical; variants in `projq.c`, `cpragma.c`, `mir_elf.c`; EV8 adds `eval.c:751`, `eval_run.c:2090`, `lsp/features.c`). Hold: a hash that is stored or compared (digests) must keep its value; check `src/hash.h`/`hash_bytes` first. About 25 lines.
- **FE10. Two `hexval` copies** (`lit.c`, `lsp/pos.c`). About 8 lines.
- **EV9. `fn_external` / `fn_name` re-inlined 16 times in `eval_run.c`** (export the two helpers through `eval_int.h`). About 20 lines.
- **FE11. 57 extern functions used only in their own file** (list in the survey). Make `static`, drop header lines. Held: `lsp.h` and `thread.h` entries may be API on purpose; decide per header.
- **FE12. Undocumented env switches** `CEREAL_TOK_STATS`, `CEREAL_PARSE_STATS` and the untested `--list-warnings`. Held: delete or document (the other `*_STATS` knobs are in docs).

### Tests, bench, tools (TS)

- **TS1. `bench/tools/perf_check.py`** (hard-coded binaries; result not recorded as a pin). 33 lines.
- **TS2. Four one-off bidi scripts** (`bidi_probe{,2,3}.py`, `bidi_chars.py`; pinned by `tests/check/bidi_*.c`, run broader by `bidi_fuzz.py`; all write `/tmp/b.c`). About 112 lines. Confirm `bidi_fuzz` covers probe1's snippets first.
- **TS3 + TS13. `mut.py` and `parse_mutate.py` are one script forked** (about 10 of 120 lines differ; `mut.py`'s docstring is the copy's), and six scripts load each other by `exec`-splitting source text. One module with `--mode check|parse`; make `corpus.py` and `par.py` importable. About 120 lines.
- **TS4. `hd.py` is superseded by `diagstat.py`** (cited only through `{par,hd,san,mut}.py` in a HISTORY note). 20 lines.
- **TS5. `bench/clangd_diff/` holds six byte-identical copies of test fixtures** (`calls.c`, `util.h`, `hints.c`, `sys/fakelib.h`, `symcalls.c`, and `tests/lsp/macsites/main.c` = `tests/query/macsites.c`); they diverge on the first single-copy edit. One home. About 4 KB. (checked, four pairs)
- **TS6. `san.py` in `tests` mode repeats what `run.sh` does under the sanitized binary** (about 640 runs per gate). Demote: keep its `corpus` and `dg` modes. Needs a seeded check that a deliberately injected ASan/UBSan report still fails the gate before removal.
- **TS7. `tests/zstd_bisect.sh`** (0 citations, a bashism under `#!/bin/sh`). 21 lines.
- **TS8. `tests/tast_gcc.py` cannot fail and runs nowhere.** Say so in its header or delete; the result is recorded (TAST_DESIGN:496, :577).
- **TS9. Root `.pt.sh`** (tracked, hard-coded WSL tree; run.sh already covers its scenarios).
- **TS10. `tests/cfg` and `tests/tast` share two identical inputs** (`cgoto.c`, `duff.c`); low value, held.
- **TS14. `s4_falsify.py`** one-off; still cited by PARKED:75 and `src/analysis/pc.c`. Demote or keep; decided against deleting.
- **TS15. Four corpus end-to-end scripts have one shape;** weak lead, not diffed. Held for a second survey.
- **TS16. `bench/BASELINE.md` first table is superseded.** Move only that table.

### Architecture (AR): each is its own plan

Ranked by end-state design, not effort. `OWN-PLAN` marks an item that changes an interface other code depends on; it leaves this loop for its own spec and plan.

- **AR2. One operator-property table plus `rel_holds(op, cmp)`.** 35 `case P_LE:` sites in 13 files; swap/negate/is-compare/assign-op/precedence/`Punct`-to-`SX_` helpers are copied across `pcexpr.c`, `cwarn_expr.c`, `cstmt_warn.c`, `cexpr.c`, `tast.c`, `parse.c`, `eval.c`. Subsumes FE6 and the relation switches; cannot subsume the arithmetic (AR3) or the two parsers. Deletes about 12 helpers and two precedence tables. All helpers are file-local, so not OWN-PLAN. **Recommended first.**
- **AR1. One builtin descriptor table** extending `BI_ROWS`: `bi_find`, `bt_find`/`cbuiltin_tab.h`, `cbuiltin_pure.h` and its generator, `builtin_nonnull`, `builtin_format_pos`/`scanf_pos`, `std_names`/`std_header`, `ev_host_fn`, the `e_call` strcmp chain. OWN-PLAN (changes `bi_find`, `bt_find`, the generated header format, the `CSym` attribute-install path). Largest end-state win. Cannot absorb the bespoke checks (`choose_expr`, `shuffle`, tgmath), which stay as handlers by id.
- **AR3. One integer-op kernel plus one canonicalizer** (`cexpr.c:174`, `eval.c:615`, `ppexpr.c:497-642`, `pcexpr.c:704`, `cexpr_trunc`/`ev_canon`). The folder-versus-evaluator split stays (policy per caller). Probably OWN-PLAN; after AR2.
- **AR4. Attribute descriptor table** (`cattr.c`: five name tables, 78 `strcmp` sites, two state owners `Attrs` and `aset`). OWN-PLAN.
- **AR5. A shared entity-query view over `Index` and `CIndex`** for `lsp/features.c` (about 20 mirrored `c_*` helpers). OWN-PLAN, medium confidence.
- **AR6. Split `cexpr.c` (7362 lines) by concern.** After AR1 so builtin pieces are not moved twice. No public interface.
- **AR7. Table-driven driver options** (`driver.c:334-477`, about 50 options; `main.c` about 36 more). Low-medium confidence; `fixmode.c` unread. Needs a second survey.
- **EV6. `eval.h` has two layers (`ev_*`, `eval_*`)** for one job; `ev_new`, `ev_add_func`, `ev_add_tentative` are used only in `eval.c`. Changes a header other files include: Decide first.
- **EV8. `fn_sync` / `sd_sync` duplicate an open-addressing index** (about 60 lines). A small shared index struct; the second copy of one shape is the tripwire. Decide first.
- **EV10. `sweep_pointee_ok` keeps the deleted fixed layout's size limit** "so the set of swept functions does not move". Simplifying may move recorded sweep counts: needs a before/after count of swept functions. Held.
- **EV17/EV18.** `fbin` float/double duplication (bit-identical required, `tests/eval` must pass); magic numbers `t->pun == 1` and the `va_*` cases. Low value.

## Considered and rejected

- Merging the checker's folder with the evaluator: decided against on record (EVAL_DESIGN section 2, PARKED).
- The `diag.c:13 options[]`, `mir.h`, `cbuiltin_x86.h` + `tast_x86.c`, the checker `e_*` dispatch, `case TY_*` switches: already tables or per-type by need.
- Deleting HISTORY or `docs/runs.md` text: history.
- `PPM_FULL`, `TY_NKINDS`, `CEREAL_LAZY_REPLAY`, `SFn.byval`, the eval CLI flags: have readers or are sentinels.
- Slimming run.sh's `par_same`/golden doubling (5 modes by 5 runs): deliberate equivalence tests; no per-test cost recorded, so no cut without timing.

## Outcomes

(Appended per item as they land.)
