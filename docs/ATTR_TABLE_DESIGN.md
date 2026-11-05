---
status: done
---

# Attribute descriptor table (CLEANUP_PLAN AR4, stage 1)

Round 1 of review (a Sonnet builder check against the code, 2026-11-04) found two header/consumer gaps, an
underspecified guard and several unstated values; all are folded in below.

## Intent

Every fact about an attribute *name* (is it known, is it standard, does it need a function type, which declaration
contexts ignore it, how many arguments, do its arguments hold expressions, is it left out of `copy`) lives today in
a separate name list, and several of those lists exist twice. Copies drift: `enumerator_attrs` in `crecord.c` has no
`format` in its function-only list, so cereal is silent on `enum { A __attribute__((format(printf,1,2))) }` where
gcc 15.2 warns "'format' attribute only applies to function types" (checked with cereal at `~/cereal-s1`,
2026-11-04; the source comments say "probed on gcc 13", and gcc 13 is not installed to compare). One descriptor row
per attribute makes each such fact exist once.

## Goals

1. One table, `src/c/attrs.def`, one row per attribute name; every per-name list below is deleted and read from the row.
2. `attr_lookup(name)` returns the row or NULL; `attr_known`/`cdecl_attr_known` are replaced by `AF_KNOWN` tests.
3. Output identical to today, except the one `format`-on-enumerator warning (s1 below).
4. A guard (below) that fails when a new hand-kept attribute-name list appears.

## Non-goals and anti-goals

- No change to `attr_collect`, `attrs_alloc_check`, `attrs_misapplied`, `attrs_merge`, `struct Attrs` or the
  diagnostics' wording and order. The handler chains stay code (handler dispatch is parked: PARKED.md "Attribute
  handler dispatch").
- No `AttrId` in `AttrState`, `CSym.aset`, `Checker.anames` or the `ign[]` filter: state keeps name strings (r1);
  ids there are parked (PARKED.md "Attribute names as ids"). Anti-goal: a second, id-keyed copy of a fact beside
  the name-keyed state. Ids are therefore never persisted or stored in state; adding a row renumbers the enum.
- `attr_excl_tab` stays where it is. It is a single copy (no drift) whose entries are compared against the string
  state, so it moves to ids only with the parked state conversion.

## Decisions already made (the user, 2026-11-04)

- q1: the table is an X-macro file `src/c/attrs.def`, as `src/diag_opts.def` is (included three times each by
  `diag.h` and `diag.c`). One text yields the `AttrId` enum, the sorted `AttrDesc` array and the lookup.
- r1: descriptor lookup first; state keeps name strings.
- s1: the `format` fix is a separate commit before the refactor, with a golden test, gated; the refactor then
  must be output-identical.

## Design

### Row

```c
ATTR(name, ID, flags, ctx, argc_lo, argc_hi)
```

- `name`: the normalised spelling (no `__x__`), rows sorted by `strcmp` (C locale; `_Noreturn` sorts first) so
  lookup is a binary search.
- `ID`: `AT_<NAME>` with the name upper-cased and a leading `_` dropped (`_Noreturn` → `AT_NORETURN2`, spelled
  by hand in the row since `noreturn` also exists). It is the row index. Call sites may use it where the name is
  already looked up; converting every `strcmp(an, "x")` is not required (the chains are untouched).
- `flags`: `AF_KNOWN` (today's `known_attrs`, 127 names), `AF_STD` (`std_known`: 6 names, copied in `cattr.c:236`
  and `parse.c:1315`), `AF_FN_ONLY` (`attr_fnonly_tab`, 27 names, now including `format`), `AF_EXPR_ARGS` (the 12
  names of `cexpr_name.c` `ex[]`: `nonnull aligned vector_size warn_if_not_aligned alloc_size alloc_align
  assume_aligned malloc fallthrough constructor destructor format_arg`), `AF_COPY_EXCL` (`copy_excluded`, 16 names).
- `ctx`: the `AC_*` contexts in which the attribute is ignored on a non-function declaration; 0 means never. A
  new bit `AC_E` (256; `AC_PUB` is 128) marks the enumerator context. Every `attr_ign_tab` name is also in
  `crecord.c` `ignored[]`, so each row of `attr_ign_tab` keeps its mask and gains `AC_E` (`common` becomes
  `AC_T|AC_P|AC_F|AC_E`); a named `AC_ALLE` (`AC_ALL|AC_E`) is used for the `AC_ALL` rows. `AC_ALL` itself stays 63:
  the `ctx == AC_T`-style tests in `attrs_ctx_check1` never see enumerators. The eleven names that are in
  `ignored[]` but not in `attr_ign_tab` get `AC_E` only: `alias cleanup error noinline packed used warning weak
  weakref` (these have an `Attrs` flag and are handled in `attrs_misapplied` elsewhere) and `mode`, `nonstring`
  (handled in `attr_collect` and `cattr.c:1800`). `enumerator_attrs` tests `ctx & AC_E`.
- `argc_lo, argc_hi`: the 11 rows of `gnu_attr_argc`'s `t[]` (`access alloc_align assume_aligned constructor copy
  destructor malloc section simd strict_flex_array zero_call_used_regs`); `AT_ARGC_ANY` (= `lo` 0, `hi`
  `UINT32_MAX`) for rows with no count rule. The `simd` argument-value check stays code.

### Lookup and spelling

`src/c/attrs.h` is a new neutral header (enum, `AttrDesc`, `AF_*`, `AC_*` moved from `cdecl_int.h`, `attr_lookup`,
`attr_lookup_n`), because `parse.c` includes only parser headers and cannot take `cdecl_int.h` (which pulls in the
whole Checker). `check_int.h` (so `cexpr_bi.c` and `cexpr_name.c`, which get it through `cexpr_int.h`),
`cdecl_int.h` and `parse.c` include it; `check_int.h`'s `cdecl_attr_known` prototype is deleted. The array and the
two functions live in `cattr.c` (the file that owns attribute semantics). `AT_*` and `AF_*` already exist in
`cintrin.h:93-98` (`AT_LOAD…`, `AF_PTR…`): `attrs.h` documents the clash risk and Task 2 checks the new names against it.

Two lookups, so no caller's behaviour moves:
- `attr_lookup(const char *name)` is exact: no stripping, NUL-terminated. It replaces `attr_known` and
  `cdecl_attr_known` for callers that already hold a normalised name (`cattr.c:642,646`, `crecord.c:904`,
  `cexpr_bi.c:522`); a doubly wrapped spelling such as `____nonnull____` stays unknown, as today.
- `attr_lookup_n(const char *s, size_t n)` takes a possibly unterminated, possibly `__x__`-spelled name and strips
  the affixes by `attr_norm`'s rule (`cattr.c:75`: `__const` → `const`, else `n > 4` and both ends `__`), then
  compares whole names (the row name must end exactly at `n`; `n == 0` gives NULL). Only `std_attr_known`
  (`parse.c`) and `attr_takes_expr` (`cexpr_name.c`) call it, replacing their two private strippers; the missing
  `__const` case there is harmless (`const` is in neither the STD nor the EXPR set). `attr_norm` stays for
  callers that need the string.
`attr_takes_expr`'s `cleanup` parameter branch stays code and `cleanup` gets no `AF_EXPR_ARGS` (it would change the
`cleanup=false` callers).

### What it replaces (and deletes)

| Today | Becomes |
|---|---|
| `known_attrs[]` + `attr_known` binary search; `cdecl_attr_known` wrapper (`cattr.c`) | `attr_lookup(..)->flags & AF_KNOWN`; both functions and the wrapper deleted, callers at `cattr.c:642,646`, `crecord.c:904`, `cexpr_bi.c:522` call the lookup |
| `std_known[]` (`cattr.c:236`; read by the loops in `std_attr_dropped` and `std_attr_unknown`, the latter through the pointer at `:259`) and `known[]` in `parse.c` `std_attr_known` | `AF_STD` (three consumers) |
| `attr_ign_tab` (`cattr.c`), `ignored[]` (`crecord.c`) | `ctx` with `AC_E` |
| `attr_fnonly_tab` (`cattr.c`), `fnonly[]` (`crecord.c`) | `AF_FN_ONLY` |
| `ex[]` (`cexpr_name.c`) | `AF_EXPR_ARGS` |
| `t[]` in `gnu_attr_argc` | `argc_lo/hi` |
| `copy_excluded` | `AF_COPY_EXCL` |

The fixed-name tests in `enumerator_attrs` (`unused`, `maybe_unused`, `nodiscard`, `malloc`, `aligned`, `section`)
are handler logic and stay.

### Subsumed and not subsumed

Subsumes: seven name sets (two duplicated across files, one in three loops) and the argument-count table, about
350 name entries in four files. Does not: the exclusion table, the ~44-name `attr_collect` chain, the other
`strcmp` chains in `attrs_ctx_check1`, `attrs_misapplied`, `attrs_alloc_check`, `attrs_copy_check`, and the state
mirror (`Attrs` flag versus `aset` name, `AttrState` pure/const).

## End-state fit

- Home: `src/c/attrs.def` + `src/c/attrs.h`, and `attr_lookup` in `cattr.c`.
- Replaces and deletes: the lists in the table above, the hand-kept sort order of `known_attrs`, both
  affix-stripping copies, and the `crecord.c` lists (the reason `enumerator_attrs` disagrees with
  `attrs_ctx_check1`).
- Adds no special case to a dispatcher: consumers read a flag where they read a list. A `_Static_assert` ties the
  row count to the enum count (as `diag.c` does).

## Verification

1. Before deleting a list, a throwaway check (not committed) dumps each old list and the set the new rows give for
   the matching flag and compares them; the only difference allowed is `AC_E` unifying the enumerator path (the
   `format` fix has already landed by s1). The old lists are in `git show HEAD:src/c/cattr.c`. Rows are the union
   of the old lists, so `AF_KNOWN` and `AF_STD` differ for `_Noreturn`, which is in `std_known` only.
2. After each consumer switch: `sh tests/run.sh` (the edit-loop check).
3. At the end: the gate (`bench/tools/gate.sh`, run via `run.ps1`), expecting run N/0, san findings 0, globals all
   allowed, verify 637/638, 289/289, 3908/3910 (the values of run `2026-11-04-ar6-inline`, the latest green gate),
   and callgrind equal to or below 3,818,395,290 (callgrind is deterministic; the linear scans over
   `attr_fnonly_tab` and `attr_ign_tab` go away, the binary search stays). The gate does not catch a change of
   check order inside `attrs_ctx_check1` (fnonly, then transparent_union, then ign); the chains are untouched, so
   none is expected.
4. s1's golden: a new `tests/check/` file with `enum { A __attribute__((format(printf,1,2))) };` whose expected
   output is gcc's wording above, written before the fix (red) and green after it.

## Guard (leaves the drift class closed)

`bench/tools/attr_lists.sh`, a new stage 0 of the gate: it prints one line `attr lists: ok` (or the failures) into
`/tmp/gate.0attrs`, exits non-zero on failure, and `gate.sh` greps that line like it greps `all allowed`.

- fails if the `attrs.def` names are not strictly `strcmp`-sorted and unique (`LC_ALL=C sort -c -u`);
- fails if any `src/c/*.c` or `src/c/*.h` file other than `attrs.def`/`attrs.h` contains a *top-level* brace
  initialiser (a `= {` ... `};` statement, so a nested row is not counted alone) that holds three or more string
  literals that are all attribute names from `attrs.def`, except entries in `bench/tools/attr_lists.allow`
  (`FILE ARRAY-NAME` per line, as `globals.allow` is `OBJECT SYMBOL`; the script fails when an entry no longer
  matches a list, so it can only shrink). The allowlist starts with `cattr.c attr_excl_tab`.

It checks array-style lists only: it does not look at `strcmp` OR-chains (`!strcmp(a,"alias") || ...`,
`cattr.c:638`), which are handler code, and it misses a list built any other way. It catches the way these copies
were made ("top-level" means the outermost `= {` of a statement, file-scope or function-local). Today it would hit
`known_attrs`, `ex[]`, `copy_excluded`, `attr_ign_tab`, `attr_fnonly_tab`, `std_known`, `known[]` in `parse.c`,
`ignored[]`, `fnonly[]`, the `t[]` of `gnu_attr_argc` and `attr_excl_tab`; the first ten are deleted by this work
and the last is the one allowlist entry. Two-name arrays elsewhere (`cmerge.c:486`, `an2`, `cn`) are below the threshold by design.

## Left to the builder

Row ordering inside macros, the exact `AF_*` bit values, whether `attr_lookup` is `static inline` in the header,
how the throwaway comparison is produced, the exact `awk` of the guard.

## Calls I made (not the user's)

- `attr_excl_tab` and the state stay strings and outside the table.
- `AC_E` is a new context bit rather than a separate enumerator table.
- The new neutral header `src/c/attrs.h`, and `attr_lookup_n` doing the affix stripping for all callers.
- The guard's detection rule (top-level initialiser, three or more attribute-name literals) and that it ignores
  OR-chains.

## Decided at approval (the user, 2026-11-05)

- v1: `attr_excl_tab` stays allowlisted until the parked ids work (PARKED.md "Attribute names as ids"), then it
  is deleted with that work.
- w1: the `format` enumerator fix stays; it matches gcc 15.2 (gcc 13, the version the code comments cite, is not
  installed), and the golden's comment names the version.
- The spec is approved. Plan: `docs/ATTR_TABLE_PLAN.md`.
