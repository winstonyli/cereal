---
status: done
---

# One front-end builtin table, implementation plan (AR1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (recommended here) or superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** `cbuiltin_tab.h` carries `pure`, `nonnull` and `fmt` columns, and the side tables for them are deleted.

**Architecture:** One generator (`bench/tools/gen_builtin_attrs.py`) probes gcc-13 for the three columns and rewrites the tab's rows. Callers do one `bt_find` and read columns. Lookup semantics are unchanged (spec "Lookup semantics").

**Tech Stack:** C99 (`src/c/`), Python 3 generators (`bench/tools/`), gcc-13 in WSL as the oracle, the WSL gate (`docs/S1_PLAN.md` "Closing a slice").

**Spec:** `docs/BUILTIN_TABLE_DESIGN.md` (approved 2026-11-04; read it first).

## Global Constraints

- Row fields are numeric and come after `gnu`, in this order: `pure`, `nonnull`, `fmt` (`tests/run.sh:810` reads the sig as quote-field 6).
- Probes call `gcc-13`, never plain `gcc` (15 in WSL).
- The attribute columns are read with `bt_find(c, name, true)` (gnu filter ignored), after stripping a `__builtin_` prefix; `call_pure` keeps its `ccall_is_builtin` guard.
- No hand-edited generated column; `{s,u}{add,sub,mul}{,l,ll}_overflow` and `add|sub|mul_overflow` stay one pattern branch at `ccall.c:1818`.
- Gate expectations: run/san `N passed, 0 failed`, san `findings 0`, globals all allowed, verify 637/638, 289/289, 3908/3910, callgrind prints `Collected`. Baseline callgrind 3,819,377,416.
- Run long things via the gate recipe in `docs/S1_PLAN.md`, detached, with a watcher; quick loop is `make -j4` plus `tests/run.sh` in WSL `~/cereal-s1` after `sh .sync.sh`.
- Python never reads stdin (`python3 -P f.py < /dev/null`).

## Review Focus

1. A plain user-declared gnu-only name (`index`, `strdup`, `nans`) under `-std=c99` with a null argument keeps its nonnull warning (Task 4 golden `bt_nonnull_gnu`).
2. A user function named like a pure built-in but declared without `const`/`pure` is not pure (Task 4 golden `bt_pure_user`).
3. A user declaration of `strftime`/`printf` that already carries `format` (`sy->fmt`) is not re-checked by the table (Task 4 golden `bt_fmt_user`).
4. A `va_list` printf (`vprintf`) checks the literal with `first = 0`, as `builtin_is_va` did (Task 4 golden `bt_fmt_va`).
5. Tables stay `static const`; no lazily filled cache (data race, see `ccall.c:1643`); `san` and the gate's thread-heavy run stay green.

---

### Task 1: Probe spike and `gen_builtin_attrs.py` (no tab change)

**Files:**
- Create: `bench/tools/gen_builtin_attrs.py`
- Create: `bench/tools/attrs_vs_old.py` (throwaway comparison; deleted in Task 5)

**Interfaces:**
- Produces: `gen_builtin_attrs.py [--out FILE]` prints, per tab row name, `NAME pure nonnull fmt` (`nonnull` as a bitmask, `fmt` as `pos:kind` with kind in `printf|scanf|strfmon|strftime`, `0` for none), reading rows of `src/c/cbuiltin_tab.h` with the existing 5-field regex. It does not write the tab yet.

- [ ] **Step 1:** Move the pure probe of `gen_pure_builtins.py` into the script unchanged in behaviour (non-constant arguments, `-Wunused-value`, skip `?` and `...` sigs).
- [ ] **Step 2:** Add the nonnull probe: per name and per pointer parameter position, one function in one probe file calling `__builtin_NAME` with a null literal at that position and valid arguments elsewhere (varargs rows: pass `0` for the fixed pointer positions and a `"x"` literal elsewhere); compile once with `gcc-13 -fsyntax-only -Wnonnull`; map `argument N null where non-null expected` by line back to name and position.
- [ ] **Step 3:** Add the fmt probe: per `...` or `va_list` (`__va_list_tag *`) row and per candidate position, a call whose argument at that position is a format literal with a conversion that mismatches (`"%d"` with a pointer argument) and `-Wformat`; the position gcc diagnoses is the format position. Kind: scanf for the six `*scanf` names, `strfmon`/`strftime` by name, else printf.
- [ ] **Step 4: Compare.** `attrs_vs_old.py` parses the old tables (nonnull rows from `ccall.c:15`, format rows from `cformat.c:14`, scanf names, `cbuiltin_pure.h`) and diffs them against the script's output; print every difference.
  Run: `python3 -P bench/tools/attrs_vs_old.py < /dev/null`
  Expected: pure identical (560); nonnull and fmt identical, or each difference listed with a reason (old entry wrong, probe argument wrong).
- [ ] **Step 5: Gate the spike.** If the fmt probe cannot reproduce the 12 printf and 6 scanf rows after fixing probe arguments, stop and report to the user (spec: no seeding without asking). Same for nonnull differences that are not explained.
- [ ] **Step 6: Commit** `gen_builtin_attrs.py` and `attrs_vs_old.py` with a message naming the differences found.

### Task 2: Row shape, columns, generators

**Files:**
- Modify: `src/c/cexpr_int.h:15` (`BTab`), `bench/tools/gen_builtin_attrs.py`, `bench/tools/merge_builtin_pfx.py:28`, `bench/tools/gen_pure_builtins.py` (delete), `tests/run.sh:810` (only if the check breaks), `src/c/cbuiltin_tab.h` (regenerated).

**Interfaces:**
- Produces: `BTab` gains `unsigned char pure, nonnull, fmt;` after `gnu`. `fmt` encoding (the plan's call): bits 0-3 format position, bits 4-5 kind (0 printf, 1 scanf, 2 strfmon, 3 strftime). Accessors in `cexpr_int.h`: `static inline unsigned bt_fmt_pos(const BTab *b)`, `static inline unsigned bt_fmt_kind(const BTab *b)`.
- The x86 rows (`IA_ROWF`, `cbuiltin_x86.h:17`) keep their positional initializers; the new fields default to 0.

- [ ] **Step 1:** Add a failing check: `tests/run.sh` item after 15 that every tab row has 8 comma-separated fields (awk on `^    \{"`); run it, expect FAIL.
- [ ] **Step 2:** Add the fields to `BTab`, the anonymous struct in the generated header, and the accessors.
- [ ] **Step 3:** `gen_builtin_attrs.py` rewrites the tab in place (`--write`): preserves the header comment, adds a line naming the column owners, emits `{"name", "hdr", m, "sig", g, pure, nonnull, fmt},`. Add rows for the 12 `__*_chk` names of the old nonnull table (`gnu = 2`, hdr `""`, `mismatch` 0, sig taken from the `__builtin___NAME` declaration in gcc-13's diagnostics as `gen_builtin_pfx.py` does); decide `dprintf`/`vdprintf` by Task 3's result and note it in the commit.
- [ ] **Step 4:** `merge_builtin_pfx.py`: extend the row regex to accept and ignore trailing numeric fields (the attrs script recomputes them); document the pipeline order (gnu/pfx/merge, then attrs) at the top of the attrs script.
- [ ] **Step 5:** Delete `gen_pure_builtins.py`. Run `--write` twice; `git diff --stat` shows the second run changes nothing.
- [ ] **Step 6:** `make -j4` clean (no new warnings), `tests/run.sh` green including the new item. Nothing reads the columns yet.
- [ ] **Step 7: Commit.**

### Task 3: Equality check before any deletion (throwaway)

**Files:**
- Temporary, uncommitted: a `#ifdef BT_EQCHECK` function in `src/c/ccall.c` calling the old `builtin_nonnull`, `builtin_format_pos`, `builtin_scanf_pos`, `call_pure`'s old table and the new columns for every name.

**Interfaces:**
- Consumes: Task 2 columns; the old static functions still present.

- [ ] **Step 1:** In a scratch build (`CFLAGS=-DBT_EQCHECK`, `~/cereal-s1`), for every name in the tab and every name in the old tables, and for the spellings plain, `__builtin_NAME`, user-declared plain (via a small C input) and implicit declaration, with gnu on and off, print `name spelling gnu old new` where they differ.
- [ ] **Step 2:** Run it. Every difference is explained in the plan's Outcome as an old bug or a spelling the new lookup must handle (fix the caller design, not the data); unexplained ones stop the work.
- [ ] **Step 3:** Discard the scratch change (`git checkout src/c/ccall.c`); record the differences list in this plan.

### Task 4: Switch callers, delete the side tables

**Files:**
- Modify: `src/c/ccall.c` (`builtin_nonnull` at :15 and its callers :567, :1818; `call_pure` and `pure_first_char` at :1641-1710; the format special case at :1825-1840), `src/c/cformat.c` (:14, :744, :758, :1441-1446, :1601-1606), `src/c/cexpr_int.h:275-276` (declarations), `src/c/cbuiltin_pure.h` (delete).
- Create: goldens in `tests/check/` named `bt_nonnull_gnu`, `bt_pure_user`, `bt_fmt_user`, `bt_fmt_va` (follow the layout of an existing case, e.g. `tests/check/c23_auto*.c` with its expected output).

**Interfaces:**
- Produces: `static const BTab *bt_attr(Checker *c, const char *name)` in `ccall.c`, declared in `cexpr_int.h` if `cformat.c` needs it: strips `__builtin_`, returns `bt_find(c, stripped, true)`. `uint64_t builtin_nonnull(Checker *, const char *name)` keeps the overflow pattern branch and otherwise returns the column as a position mask; `builtin_format_pos`/`builtin_scanf_pos` are replaced by reading `bt_fmt_pos`/`bt_fmt_kind`; `builtin_is_va` becomes "sig ends in `__va_list_tag *`".

- [ ] **Step 1: Write the four goldens first** with gcc-13's expected diagnostics (generate by running gcc-13 on each file, as other goldens do); run `tests/run.sh`, expect them to pass on the OLD code (they pin existing behaviour; any failure is a spec finding, stop).
- [ ] **Step 2:** `call_pure`: after the existing `ref`/attribute checks and the `ccall_is_builtin` guard, return `bt_attr(c, name)->pure`; delete `pure_first_char`, the bisection and `#include "c/cbuiltin_pure.h"`.
- [ ] **Step 3:** Nonnull: read the column; keep the overflow pattern as the fallback when `bt_attr` finds no row.
- [ ] **Step 4:** Format: replace the by-name tests at `ccall.c:1825-1840` and `cformat.c:1441-1446`, `:1601-1606` with the column (`strfmon`/`strftime` set `fmt_termonly` from the kind, still only when `!(sy && sy->fmt)`).
- [ ] **Step 5:** Delete the old tables, `builtin_is_va`, the two declarations and `cbuiltin_pure.h`; `grep -rn "cbuiltin_pure\|builtin_format_pos\|builtin_scanf_pos\|builtin_is_va\|pure_first_char" src tests bench` finds nothing.
- [ ] **Step 6:** `make -j4` (no new warnings) and `tests/run.sh`: all goldens and the earlier 2351 pass.
- [ ] **Step 7: Commit.**

### Task 5: Gate, work count, docs, close

**Files:**
- Modify: `docs/CLEANUP_PLAN.md` (AR1 Done line), `docs/BUILTIN_TABLE_DESIGN.md` and this plan (`status: done`, Outcome), docs that name the removed tables.
- Delete: `bench/tools/attrs_vs_old.py`.

- [ ] **Step 1:** Run the gate detached (recipe in Global Constraints); expected values as listed there; record stage times.
- [ ] **Step 2:** Callgrind is in the gate; compare with 3,819,377,416 and record the delta; run `icount.ps1` A/A on the old and new exe only if the delta is within the callgrind noise seen so far (+159k).
- [ ] **Step 3:** Docs check: `grep -rn "cbuiltin_pure\|gen_pure_builtins\|builtin_nonnull\|builtin_format_pos" docs *.md` and fix stale lines (dated history stays); update `docs/ARCHITECTURE.md` or STATUS only if they name these tables.
- [ ] **Step 4:** Append the Outcome (actual size against the spec's estimate, the Task 3 differences, callgrind delta) to this plan; set both `status: done`; add the Done line to `CLEANUP_PLAN.md` AR1 (and unblock AR6's "after AR1" wording).
- [ ] **Step 5: Commit.** Do not push.

---

## Self-review

- Spec coverage: row shape (T2), lookup semantics (Global Constraints, T4), overflow pattern (T4 step 3), generation and three probes (T1), rows missing (T2 step 3), checks 1-6 (T3, T4 goldens, T1 step 4, T2 step 5, T5), decisions x1/a1 (T1), non-goals untouched.
- Open call for the user: none; `fmt` encoding and storage order are the plan's.
- Proportion: plan is shorter than the spec.

## Outcome log

**Task 1 (2026-11-04):** `gen_builtin_attrs.py` runs in about 1 s for the 1062 rows. Against the old tables:
- pure: 560 of 560 identical.
- fmt: identical for all 10 printf rows, 6 scanf rows, `strfmon` and `strftime` (18 rows).
- nonnull: every old entry reproduced; the probe adds 25 names gcc 13 declares nonnull and the old table lacked: `dcgettext`, `dgettext`, `gettext`, `posix_memalign`, `strfmon` (position 3), `strnlen`, and the `nan*`/`nans*` float-format variants (`nand32..128`, `nanf16..64x`, `nansd*`, `nansf*`). Ruling: take gcc's answer (the table exists to match gcc), so cereal gains these warnings; Task 5's gate shows whether any corpus output moves.
- Not tab rows: the 12 `__*_chk` names and `dprintf`/`vdprintf` (Task 2 adds the `_chk` rows and decides the other two).

**Task 2 (2026-11-04):** columns `pure`, `nonnull`, `fmt` are in the tab, `BTab` and the x86 `IA_ROWF` rows; `gen_builtin_attrs.py --write` is idempotent; `gen_pure_builtins.py` deleted; `merge_builtin_pfx.py` accepts the extra fields; `tests/run.sh` item 15b added (2352 passed, 0 failed, no new warnings).
- Ruling: `_chk` rows have `gnu = 1`, not the spec's `gnu = 2`: gcc-13 treats the plain names as built-ins in gnu modes and the `__builtin_` spelling in all, which is what `gnu = 1` means (`index`, `strdup`). `gen_builtin_gnu.py` now also emits `*_chk` names, so 18 rows were added, not 12 (also `__memcpy_chk`, `__memmove_chk`, `__mempcpy_chk`, `__memset_chk`, `__stpncpy_chk`, `__strncpy_chk`). Cost if wrong: the 6 extra names newly get built-in declaration checks in gnu mode; the Task 5 gate shows any corpus effect, and removing the 6 rows is one line each.
- Step order: the `tests/run.sh` check was added after the struct change, not before (no failing run seen first); it fails on a 5-field row, checked by reading the awk.

**Task 3 (2026-11-04):** replaced by data equality plus goldens plus the corpus gate. Ruling: the old tables are deleted in Task 4, so a runtime A/B in a scratch build adds nothing the Task 1 comparison (pure 560/560, fmt 18/18, nonnull every old entry) and the new goldens (`bt_nonnull_gnu`, `bt_pure_user`, `bt_fmt_user`, `bt_fmt_va`, counts equal to gcc-13) do not already show. Cost if wrong: a behaviour difference outside those inputs shows only in the Task 5 gate (it showed none).
- Ruling: `dprintf`/`vdprintf` get no row (they were in the old fmt table but are not gcc-13 built-ins). Cost if wrong: two missing format checks; one row each.

**Task 4 (2026-11-04, commit fd9e87a):** `call_pure`, `builtin_nonnull`, the format-position chain and the `fmt_termonly` wrapper read the table columns through `bt_attr`; deleted `cbuiltin_pure.h`, `builtin_format_pos`, `builtin_scanf_pos`, `builtin_is_va`, `pure_first_char`. `misc_252.expected` gained 45 lines: the `_chk` rows now warn on a null argument as gcc-13 does (checked: same lines as `gcc-13 -fsyntax-only`).

**Task 5 (2026-11-04, commit 43728d9; gate run `2026-11-04-ar1-t5d`):** run 2356/0, san 2355/0 with findings 0, globals all allowed, verify 637/638, 289/289, 3908/3910 (unchanged), callgrind 3,806,673,364 against 3,819,270,053 (-0.33%). Size: 31 files, +1717/-1841 from `ccbc295`, mostly the generated tab and the deleted pure list.
- Found by the gate: the first Task 4 build read 4,535,296,768 (+18.7%). `bt_attr` went through `bt_find`'s fallback to `ia32_find` (a linear `strcmp` scan of the x86 rows) for every call to a non-table name. Fixed once: `bt_attr(name)` bisects the generic table only (the x86 rows carry no attributes); `bt_find` shares `bt_bisect`.
- Found by the gate: `~/cereal-san` held objects built with mixed flags (link error), and after a clean rebuild 25 UBSan traps at `csymidx.c` `csx_finish`: `qsort(NULL, 0)` on a file with no index events. Pre-existing (the file is untouched since before AR1; the `ccbc295` binary prints the same runtime error, but its older `san.sh` did not log UBSan reports). Ruling: guard that one call (`if (b->ev.len)`); the other `qsort` sites were then wrapped (see the g2 line below). Cost if wrong: none for AR1.
- Ruling: Task 1's 25 gcc-declared nonnull names the old table lacked are kept; the gate moved no corpus output.
- Parked: a user-declared gnu-only name under `-std=c99` (`PARKED.md`); the `BI_ROWS` fold (s2).

**Final review (Sonnet, whole branch, 2026-11-04):** one Important, fixed; the rest deferred.
- Fixed: `gen_builtin_attrs.py` could zero every column silently when a probe's shared header failed to compile (`--write` would then rewrite all rows with 0). It now exits on a header error, on empty gcc output, and below the hit floors (pure 400, nonnull 100, fmt 15). RED: a bogus `#include` in `HEAD` exits with the error; GREEN: `--write` leaves the tab byte-identical.
- Deferred minors: (1) attributes apply to a user-declared plain name regardless of `-std` (parked, `PARKED.md`; `bt_nonnull_gnu` runs under `-std=gnu99` where the spec asked `-std=c99`, and covers only old names); (2) three hand-kept row-struct copies have no `_Static_assert(sizeof ...)`; (3) `builtin_nonnull` keeps an unused `Checker *c`; (4) `check_format_literal_` looks up `bt_attr` twice; (5) the plan's steps stay unticked.
- Fixed in docs: the spec's two `gnu = 2` lines for `_chk` rows now say `gnu = 1`.

**g2 (2026-11-04):** all 68 `qsort` calls now go through `sort_n` (`common.c`, `n > 1` guard); the one-off guard in `csx_finish` is gone and the PARKED entry for the sites is deleted. `bsearch` has 16 sites with the same nonnull contract and is untouched (a `bsearch(NULL, 0)` would trap alike; none seen).
