---
status: draft
---

# Attribute descriptor table, implementation plan (AR4)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (inline, one implementer). Steps use checkbox (`- [ ]`) syntax.

**Goal:** `src/c/attrs.def` holds one row per attribute name, and the per-name lists in `cattr.c`, `crecord.c`, `parse.c` and `cexpr_name.c` are deleted.

**Architecture:** An X-macro file yields the `AttrId` enum, a strcmp-sorted `AttrDesc` array and `attr_lookup`/`attr_lookup_n` (in `cattr.c`, declared in the new neutral `attrs.h`). Each consumer switches from a list to a flag in one task, old list deleted in the same task. Handler chains and name-string state are untouched.

**Tech Stack:** C99 (`src/c/`), sh/awk guard (`bench/tools/`), the WSL gate (`docs/S1_PLAN.md` "Closing a slice").

**Spec:** `docs/ATTR_TABLE_DESIGN.md` (approved 2026-11-04 with v1 and w1; read it first).

## Global Constraints

- Output identical to the pre-task tree on the gate, except Task 1's one new warning.
- Quick loop in WSL: `sh ~/cereal-s1/.sync.sh` (rsync + make), then `sh tests/run.sh` there. Gate, detached with a watcher: `powershell -NoProfile -File "$USERPROFILE/.claude/runs/run.ps1" -Kind gate -Name <n> -Detach -Command "wsl -e sh /home/winstonyli/rungate.sh"`.
- Gate expectations: run/san `N passed, 0 failed` (N = old + new goldens), san `findings 0`, globals all allowed, verify 637/638, 289/289, 3908/3910, callgrind `Collected` at or below 3,818,395,290 (run `2026-11-04-ar6-inline`).
- Rows keep the C-locale `strcmp` order; `AC_ALL` stays 63; `AC_E` is 256.
- Python never reads stdin; scratch scripts go in the scratchpad, never the repo.

## Review Focus

1. `format` on an enumerator warns once; `nonnull` still does (Task 1 golden).
2. `__x__` spellings (`__nonnull__`, `__maybe_unused__`, `__const`) are found by `attr_lookup_n` where the old strippers found them, and a doubly wrapped `____nonnull____` stays unknown (Task 3 golden `attr_spell.c`, through `__builtin_has_attribute`, not the preprocessor's `__has_attribute`, which uses `pp->host_attrs`).
3. `cleanup(undeclared)` stays silent on the `cleanup=false` path, `__cleanup__(fn)` still records a use on the `true` path, and `AF_EXPR_ARGS` names (`__nonnull__(undeclared)`, `format_arg`) still check their arguments as expressions (Task 5 golden `attr_expr_args.c`).
4. `[[unknown(1 +)]]` is skipped as balanced tokens and `[[__deprecated__("m")]]`, `[[_Noreturn(1)]]` parse as known, with parentheses so `std_attr_known` runs (Task 3 golden `attr_std_c99.c`).

## End-state fit

Home: `src/c/attrs.def`, `src/c/attrs.h`, `attr_lookup`, `attr_lookup_n` in `cattr.c`, guard in `bench/tools/attr_lists.sh`. Replaces the lists named in the spec's table and deletes them task by task; nothing is kept beside the new table. Allowlist: `cattr.c attr_excl_tab` only.

---

### Task 1: `format` on an enumerator (spec s1, commit before the refactor)

**Files:**
- Create: `tests/check/enum_attr_format.c`, `tests/check/enum_attr_format.expected`
- Modify: `src/c/crecord.c` (`fnonly[]` in `enumerator_attrs`, add `"format"` in sorted position)

- [ ] **Step 1: Write the case.** `tests/check/enum_attr_format.c`: first line `// flags: -std=gnu99 -Wall`, then a comment saying the warning matches gcc 15.2, then `enum { A __attribute__((format(printf, 1, 2))), B __attribute__((nonnull)) };`.
- [ ] **Step 2: Hand-write `.expected`, watch it fail.** A golden is `cat stderr stdout` of `cereal -fsyntax-only -std=c99 -pedantic <flags line> file.c` run from `tests/check` (`check_golden`, `tests/run.sh:353-367`), with file-relative locations plus source line and caret. Write the two warnings by hand (`'format' attribute only applies to function types [-Wattributes]`, then `'nonnull' ...`), shaped like `bt_nonnull_gnu.expected`. In `~/cereal-s1` run `sh tests/run.sh`. Expected: a mismatch for this case only.
- [ ] **Step 3: Add `"format"` to `fnonly[]`** in `enumerator_attrs`.
- [ ] **Step 4: `sh .sync.sh`, then `sh tests/run.sh`.** Expected: case matches, `N+1 passed, 0 failed`.
- [ ] **Step 5: Gate it** (spec s1: "gated"): run the gate, detached. Expected per Global Constraints; the callgrind total is unchanged.
- [ ] **Step 6: Commit** `Warn on 'format' applied to an enumerator`. Also set this plan and the spec to `status: active`.

### Task 2: the table, unused

**Files:**
- Create: `src/c/attrs.def`, `src/c/attrs.h`
- Modify: `src/c/cattr.c` (array, `attr_lookup`, `attr_lookup_n`), `src/c/cdecl_int.h` and `src/c/check_int.h` (include `attrs.h`; move the `AC_*` enum from `cdecl_int.h:190` to `attrs.h`, add `AC_E`, `AC_ALLE`)

**Interfaces:**
- Produces: `AttrDesc { const char *name; id; flags; ctx; argc_lo; argc_hi; }` (field widths the builder's choice, but `ctx` must hold 256 and `argc_hi` must hold `AT_ARGC_ANY`; a `uint8_t` ctx silently drops `AC_E`); `const AttrDesc *attr_lookup(const char *name)` (exact, no stripping); `const AttrDesc *attr_lookup_n(const char *s, size_t n)` (strips `__x__` by `attr_norm`'s rule incl. `__const`, whole-name compare, `n == 0` → NULL); `AF_KNOWN, AF_STD, AF_FN_ONLY, AF_EXPR_ARGS, AF_COPY_EXCL`; `AT_ARGC_ANY`; `_Static_assert` row count equals enum count. Step 2 also greps the new `AT_*`/`AF_*` names against `cintrin.h:93-98` (no clash today).
- Consumes: the old lists, read from `git show HEAD:src/c/cattr.c` etc.

- [ ] **Step 1: Write the throwaway comparison first** (scratchpad `attr_cmp.sh`): extracts the old `known_attrs`, `std_known`, `attr_ign_tab` (name,mask), `attr_fnonly_tab`, `ex[]`, `copy_excluded`, `t[]` and `crecord.c` `ignored[]`/`fnonly[]` from the tree at the Task 1 commit, and prints per-flag name sets from `attrs.def`. Run it before the rows exist. Expected: every set reports "missing".
- [ ] **Step 2: Write `attrs.def` and the array.** One row per name in the union of the lists (spec "Row"); `AC_E` added to every `attr_ign_tab` mask, `AC_E` alone for the 11 names the spec lists; `format` already `AF_FN_ONLY`.
- [ ] **Step 3: Run the comparison.** Expected: per-flag sets equal the old lists, the only difference `AC_E` and `_Noreturn` in STD without KNOWN.
- [ ] **Step 4: `make` and `sh tests/run.sh`.** Expected: `N+1 passed, 0 failed` (nothing reads the table yet; `-Wunused` clean).
- [ ] **Step 5: Commit** `Add the attribute descriptor table (unused)`.

### Task 3: known and standard names

**Files:** Modify `src/c/cattr.c` (`known_attrs`, `attr_known`, `cdecl_attr_known`, `std_known` and the loops in `std_attr_dropped`/`std_attr_unknown`), `src/c/parse.c` (`std_attr_known`), `src/c/crecord.c:904`, `src/c/cexpr_bi.c:522`, `src/c/cdecl_int.h:219` and `check_int.h:824` (delete the prototypes). Create: `tests/check/attr_spell.{c,expected}`, `tests/check/attr_std_c99.{c,expected}`.

- [ ] **Step 1: Write the two cases and record today's output** as `.expected` (it must be identical before and after this task). `attr_spell.c` (`// flags: -std=gnu99 -Wall`): `__attribute__((__nonnull__))`, `__attribute__((__const))` and `____nonnull____` spellings on a function and a variable; `__builtin_has_attribute(f, __maybe_unused__)`, `(f, nodiscard)`, `(f, ____nonnull____)` (the checker path `cexpr_bi.c:502-528`; the preprocessor's `__has_attribute` never reaches the table). `attr_std_c99.c` (default `-std=c99 -pedantic`, no flags line needed): `[[unknown(1 +)]]`, `[[__deprecated__("m")]]`, `[[_Noreturn(1)]]`, `[[nodiscard]]`, each with parentheses so `std_attr_known` (`parse.c:1358`) runs.
- [ ] **Step 2: Replace** every `attr_known(x)`/`cdecl_attr_known(x)` by `attr_lookup(x)` tests on `AF_KNOWN` (a `static inline bool attr_is_known(const char *)` in `attrs.h` is fine); `std_attr_known` and the two `std_known` loops use `AF_STD` through `attr_lookup_n`. Delete `known_attrs`, `std_known`, both functions, the wrapper and the `parse.c` `known[]`.
- [ ] **Step 3: Run `sh tests/run.sh`** after `.sync.sh`. Expected: `N+k passed, 0 failed`, new goldens unchanged from Step 1.
- [ ] **Step 4: Commit** `Read known and standard attribute names from the table`.

### Task 4: function-only and ignored contexts

**Files:** Modify `src/c/cattr.c` (`attr_fnonly_tab`, `attr_ign_tab`, the loops at `attrs_ctx_check1`), `src/c/crecord.c` (`enumerator_attrs`: delete `ignored[]`, `fnonly[]`, use `AF_FN_ONLY` and `ctx & AC_E`).

- [ ] **Step 1: Run `sh tests/run.sh` on the Task 3 tree**; note `N`.
- [ ] **Step 2: Switch the loops** (order inside `attrs_ctx_check1` and `enumerator_attrs` unchanged: function-only before ignored); delete the four lists.
- [ ] **Step 3: `sh tests/run.sh`.** Expected: `N passed, 0 failed`, Task 1's case still green.
- [ ] **Step 4: Commit** `Read function-only and ignored attribute contexts from the table`.

### Task 5: expression arguments, argument counts, copy exclusion

**Files:** Modify `src/c/cexpr_name.c` (`attr_takes_expr`: its own `__` strip goes, so look the row up first, `d = attr_lookup_n(an, al)`, and keep the `cleanup` branch as `if (cleanup && d && d->id == AT_CLEANUP) return true;`; `cleanup` gets no `AF_EXPR_ARGS`; `ex[]` → `d->flags & AF_EXPR_ARGS`), `src/c/cattr.c` (`gnu_attr_argc` `t[]` → row `argc_lo/hi`, `copy_excluded` → `AF_COPY_EXCL`). Create: `tests/check/attr_expr_args.{c,expected}`. The existing `attr_argc.c`, `attr_copy_excl.c`, `attr_copy_errs.c` cover counts and `copy`; check they list every one of the 11 counted names and add a line to `attr_argc.c` for any missing.

- [ ] **Step 1: Record today's output** for `attr_expr_args.c` (`-std=gnu99 -Wall`): `cleanup(undeclared)` (silent path), `__cleanup__(fn)` with a declared `fn`, `__nonnull__(undeclared)`, `format_arg(undeclared)`. `.expected` from the Task 4 tree; commit the case with it.
- [ ] **Step 2: Switch the three consumers; delete `ex[]`, `t[]`, `copy_excluded`'s array.**
- [ ] **Step 3: `sh tests/run.sh`.** Expected: green, goldens unchanged.
- [ ] **Step 4: Commit** `Read attribute argument counts, expression arguments and copy exclusion from the table`.

### Task 6: the guard

**Files:** Create `bench/tools/attr_lists.sh`, `bench/tools/attr_lists.allow` (`cattr.c attr_excl_tab`). Modify `bench/tools/gate.sh` (stage `0attrs`, grep the success line, fail on missing).

- [ ] **Step 1: Write the script** to the spec's "Guard" rule (sorted-unique check on `attrs.def`; outermost-initialiser scan of `src/c/*.c` and `*.h`; the array name comes from the `NAME[] = {` text, and for `attr_excl_tab` from the `} attr_excl_tab[] = {` line of a multi-line statement; stale-allowlist failure; prints `attr lists: ok`). Note `gate.sh` pipes stage output through `tail`, so the success line, not the exit code, is what the gate checks.
- [ ] **Step 2: Watch it fail.** Make a scratch worktree of the Task 1 commit, copy in `attrs.def`, `attr_lists.sh` and the allow file, and run it. Expected: eleven hits (the ten lists of the spec plus `attr_excl_tab`). Then break the scratch copy three ways, each must fail: an unsorted `attrs.def` row, a duplicate row, an allowlist entry matching nothing.
- [ ] **Step 3: Run it on the current tree.** Expected: `attr lists: ok`.
- [ ] **Step 4: Wire into `gate.sh`** like `globals` (stage `0attrs`, grep for `attr lists: ok`, set `bad=1` otherwise); add a one-line header comment.
- [ ] **Step 4: Commit** `Guard against new hand-kept attribute name lists`.

### Task 7: gate and docs

- [ ] **Step 1: Run the gate** (detached, watcher). Expected per Global Constraints.
- [ ] **Step 2: Docs.** `docs/CLEANUP_PLAN.md` AR4: Done with date, commits, size against estimate, gate numbers (its "draft" wording at line 84 goes); add an `attrs.def`/`attrs.h` line to `docs/ARCHITECTURE.md` (it has no attribute entry today); this plan and the spec to `status: done`; `docs/STATUS.md:36` `tests/check` file count; `PARKED.md` evidence line `cdecl_int.h:193`; `docs/runs.md` is written by the wrapper; grep the docs for "known_attrs", "attr_ign_tab", "std_known", "attr_known" and fix stale text.
- [ ] **Step 3: Commit** `Close AR4`. Whole-branch review by one Sonnet reviewer (under ~300 changed lines), fixes, then stop; no push.
