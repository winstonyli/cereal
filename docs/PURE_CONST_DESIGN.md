---
status: draft
---

# Pure/const state in the name sets (AR4 stage 3)

Follows `docs/ATTR_IDS_DESIGN.md` (stage 2, done). Scope chosen by the user 2026-11-05: pure/const only (the s1 option);
the other `CSF_*` attribute mirrors and the `Attrs` bools stay, with the evidence below.

## Problem

"This function is pure / const" is held three times:

1. `AttrState` (`cdecl_int.h`): `pure`, `cnst`, `pure_from`, `cnst_from`, `pure_loc`, `cnst_loc`, used only by
   `attr_excl` and `attrs_state_init` (`cattr.c`).
2. `CSym.flags`: `CSF_PURE`, `CSF_CONSTFN`, written from (1) at `cdecl.c:2411` and `cdecl.c:3361`, read by
   `attrs_state_init`, the `copy` branch of `attrs_copy_check`, `cmerge.c:725` (merge) and `cwarn_expr.c:912`
   (call equality).
3. The `AT_PURE` / `AT_CONST` entries of `CSym.aset`, which hold the same fact and survive redeclaration merges
   already (`attrs_merge`).

The conflict check for the pair is also done twice: `attr_excl_generic` skips the pure/const pair with a `continue`
(`cattr.c`, "attr_excl") and `attr_excl` (about 25 lines) re-implements it with its own state. It is the only attribute
pair outside the generic table, and `copy` of a function handles it by flag where `copy` of an object (common/nocommon)
uses the generic mechanism (`copy_set`, `copy_loc`, `cdecl_aset_has(r->aset, ...)`).

## Design

Place: all of it in `cattr.c` (the exclusion machinery) plus the three flag readers. No new module.

1. **Exclusion.** `attr_excl_generic` handles pure/const like every other pair: the `continue` stays only as a kind guard
   (the pair is checked by gcc's handlers, so only for functions: today `int v __attribute__((pure, const))` prints
   two "attribute ignored" warnings and no conflict; keep that). `attr_excl` and its call at the `kind == CS_FUNC &&
   (AT_PURE || AT_CONST)` branch are deleted. The "previous declaration here" note comes from `st->prevloc` (previous
   declaration, via `pset`) or `st->copy_loc` (via `copy`), the two sources the generic path already has.
2. **Copy.** The function branch of `attrs_copy_check` and the object branch become one loop over a per-kind list of ids
   (function: `AT_PURE`, `AT_CONST`; object: `AT_COMMON`, `AT_NOCOMMON`) using `cdecl_aset_has(r->aset, id)`,
   `copy_set`/`copy_loc` and `attr_excl_generic`. `st->nonnull |= r->nonnull` stays in the function case.
3. **State.** `AttrState` loses its six fields; `attrs_state_init` loses the flag seeding.
4. **Flags.** `CSF_PURE` and `CSF_CONSTFN` are deleted (enumerators, the two writes, the merge mask, the `copy` reads). The
   call-equality reader asks `cdecl_aset_has(c, csym(c, ref)->aset, AT_PURE)` and `AT_CONST`. Dropped entries are
   `AT_NONE` tombstones, so an excluded `pure` is not found, as the flag was not set. The other enumerator values keep
   their numbers (gaps stay; no renumbering).
5. **Guard** (WORKING_STYLE "a cleanup leaves a guard"): `bench/tools/attr_lists.sh` gains a check that fails on a
   `CSF_*` enumerator whose comment names `__attribute__((x))` with `x` a row of `attrs.def`, unless listed in
   `bench/tools/attr_lists.allow`. The seven that remain are the allowlist: `CSF_NORETURN`, `CSF_WEAK`,
   `CSF_ATTR_UNUSED`, `CSF_DEPRECATED`, `CSF_UNAVAILABLE`, `CSF_GNU_INLINE`, `CSF_WEAKREF`. It can only shrink.

## Found, kept (evidence, 2026-11-05)

- The seven kept flags are not plain mirrors: `CSF_NORETURN` also comes from `_Noreturn` (`cdecl.c:1330-1334`) and feeds
  `SF_NORETURN` (`csum.c:272`); `CSF_DEPRECATED`/`CSF_UNAVAILABLE` use an exclusive encoding (`unavailable` wins,
  `cdecl.c:2483`), are also stored in `dep` on records, fields and enumerators (`crecord.c:773`, `1066`, `1354`) and pair
  with `dep_msg`; `CSF_WEAKREF` implies `CSF_WEAK` (`cdecl.c:2497`). Folding them would add a second encoding.
- `Attrs` bools and `aset` entries are not a synced pair: `aset` is built from the attribute nodes by
  `cdecl_attrs_names`, `Attrs` by `attr_collect`; the tombstone golden (`attr_ids_tombstone`) pins that they differ
  (written versus kept).

## Risks and checks

- **Order of reports.** The generic table reports the first matching exclusion in table order (for `pure`: `const`
  first). Before, the pair was reported after every other exclusion. They differ only when a declaration conflicts both
  with the pair and with another attribute, which needs a previous declaration holding `noreturn`/`malloc`/`alloc_*`
  together with a `const`/`pure` that conflicts, and those exclude each other, so I expect no reachable case. The
  existing goldens (`attr_excl`, `attr_copy_excl`, `attr_pure_const_void`) and the corpus diff check it.
- **Merge.** `cmerge.c` ORed the flags from the old symbol; the merged `aset` must hold the same. A new golden redeclares a
  pure function without the attribute and calls it twice with equal arguments (the equality warning needs the fact).
- **`copy`.** A new golden copies a pure function onto a declaration with `const` (conflict, note at the source) and copies
  a function with excluded (tombstone-only) `pure`.
- **Variables.** A golden keeps `int v __attribute__((pure, const))` free of a conflict.
- Verification: the existing suite passes unchanged apart from the new goldens (each new golden file adds 2 passes);
  per-golden mutations (revert each of the four changes in a scratch copy and see the new golden fail); gate green,
  callgrind total not worse (the baseline is recorded in the plan).

## Non-goals

The seven kept flags, `Attrs`, handler dispatch, the second pure/const-like pair candidates (none found).

## Docs to update

`PARKED.md` (the stage-3 entry is replaced by the kept-flags evidence as `decided against`), `docs/CLEANUP_PLAN.md` (AR4
line), `docs/STATUS.md` (golden count), `docs/ATTR_IDS_DESIGN.md` (no change: it names the stage-3 entry).
