---
status: draft
---

# Attribute names as ids in checker state (AR4 stage 2)

Spec for the parked "Attribute names as ids in `AttrState` and `aset`" entry (PARKED.md; origin docs/CLEANUP_PLAN.md AR4,
whose design is docs/ATTR_TABLE_DESIGN.md). Scope chosen by the user 2026-11-05 (j1): the name strings in `AName`,
`AttrState.cur` and `Checker.ign`; not the `pure`/`cnst` and `Attrs`-flag duplicates (stage 3, stays parked).

## Intent

AR4 stage 1 made `src/c/attrs.def` the one home of per-name facts, but the checker still keeps attribute *names* as
strings in three stores and compares them with `strcmp` against literals: `AName.name[32]` (the entries of the sets
behind `CSym.aset`), `AttrState.cur[24][24]` and `Checker.ign[8][24]`. `attr_excl_tab` is the one hand-kept name list
left (allowlisted in `bench/tools/attr_lists.allow`). Holding `AttrId` in those stores drops the string copies and
compares, lets `attr_excl_tab` use enumerators, and empties the allowlist.

## Goals

- `AName`, `cur` and `ign` hold ids; `aset_*`/`cdecl_aset_*` take ids. No attribute-name string is stored in checker state.
- `attr_excl_tab` rows are enumerators; the allowlist entry is deleted and the guard passes with an empty allowlist.
- Output is byte-identical on the whole corpus: same gate counts, no golden changes.

## Non-goals

`AttrState.pure/cnst` and the `Attrs`-flag versus `aset` pairs; handler dispatch in `attr_collect`; the string-keyed
helpers that parse source text (`attrs_item_named`, `attr_arg_eq`, `cdecl_attr_name`, `cdecl_attr_args`); diagnostics
wording (they print `attr_name(id)`, the row's name).

## Behaviour that must be preserved (what the review found)

The set machinery is observable through more than names, so each of these is pinned:

1. **Unknown names keep an entry.** `cdecl_attrs_names` adds every attribute, known or not. Whether a set *exists*
   (`aset != 0`) and how many entries it has changes behaviour: `cexpr_asets` has 3 slots (cexpr_bi.c:481-498), a
   typedef with only an unknown attribute pushes a `tdas` entry (cdecl.c:2651-2657), `if (o->aset)` at cmerge.c:748.
   So a name with no row becomes an entry with `id = AT_UNKNOWN` and no string. No reader ever matches an unknown
   name (`e_has_attr` rejects non-`AF_KNOWN` names before querying, cexpr_bi.c:522-527; every literal reader uses a
   table name). `aset_add` does not de-duplicate `AT_UNKNOWN` entries against each other (today two unknown names
   differ, one repeated unknown name merges); the builder greps that no reader counts entries.
2. **Dropped entries stay entries.** `aset_drop`/`aset_keep_max_aligned` turn an entry's name into the empty string; it is
   still copied by `aset_copy` (since `copy_excluded("")` is false) and de-duplicated against other empties. With ids the
   entry keeps its place with `id = AT_NONE`, `aset_add(AT_NONE, arg)` is legal, and `aset_copy` copies it. Nothing
   skips `AT_NONE` entries.
3. **`cur` and `ign` push unknown names too.** `attr_excl_generic` pushes every attribute into `cur` up to 24
   (cattr.c:2633); it keeps pushing `AT_UNKNOWN`, so the cap behaves as today (24 unknown, then `pure`, then `const`).
4. **Chain order.** `aset_copy` walks `anames` oldest first; `aset_opt_args` (cmerge.c:650) and `first_arg` read the
   chain newest first. Neither changes.
5. **Truncation is unobservable.** Seven table names are 24-30 characters (`callee_pop_aggregate_return`,
   `no_address_safety_analysis`, `no_caller_saved_registers`, `no_profile_instrument_function`,
   `patchable_function_entry`, `transaction_may_cancel_outer`, `transaction_safe_dynamic`); `cur`/`ign` cut at 23, but
   only `attr_excl_tab` names (all 18 or fewer) are ever written to `ign` and compared in `cur`, so the cut never
   decided a comparison. Ids remove the cut; no behaviour changes.

## Design

- `attrs.h`: `typedef enum AttrId AttrId;` `AT_NONE = 0` first, rows in `attrs.def` order after it (so `AT_NORETURN2`,
  the row `_Noreturn`, is 1 and `AT_ACCESS` is 2), then `AT_UNKNOWN`, then `AT_COUNT_`. Neither is a row.
  `attr_lookup` bisects over `sizeof attr_tab / sizeof *attr_tab`, not `AT_COUNT_` (cattr.c:316 today). The
  `_Static_assert` compares that count with `AT_UNKNOWN - 1`. `attrs.def`'s header comment ("the row for AT_X is
  attr_tab[AT_X]") becomes "attr_tab[AT_X - 1]".
- `const AttrDesc *attr_by_id(AttrId)` (rows only, NULL for `AT_NONE`/`AT_UNKNOWN`) and `const char *attr_name(AttrId)`
  (its name, "" for `AT_NONE`, "?" for `AT_UNKNOWN`: never printed) in `cattr.c`, declared in `attrs.h`. `copy_excluded`
  becomes `attr_by_id(id)->flags & AF_COPY_EXCL`: no string bisect on that path.
- `AName { uint32_t set, prev; uint16_t id; char arg[24]; }` (`AttrDesc.id` is `uint16_t`; `AttrId` converts at the field).
- `aset_add(c, &set, AttrId, arg)`, `aset_drop`, `cdecl_aset_has`, `cdecl_aset_first_arg` take `AttrId`. Callers holding
  source text call `id = attr_lookup(an) ? attr_lookup(an)->id : AT_UNKNOWN` once, after `attr_norm`: `cdecl_attrs_names`,
  `e_has_attr` (cexpr_bi.c:615,624, which also keeps `an` for its diagnostics), `attrs_copy_check`. A helper
  `AttrId attr_id_of(const char *normalised)` in `cattr.c` wraps that.
- Literal call sites become enumerators: the 14 outside `cattr.c` (ccall.c 2, cdecl.c 4, cexpr_alias.c 1, cexpr_bi.c:451,
  cgx86.c 2, cmerge.c 1, crecord.c 1, cstmt.c 1, cwarn_expr.c 1) plus those inside it (`aset_add "access"`
  cattr.c:2125, `aset_drop` cdecl.c:2650 and 2669, `aset_add "noreturn"` cdecl.c:2663 and 3516, `aset_opt_args` at
  cmerge.c:651, `acc_replay_ref` cattr.c:2055, the `aligned` loops, `alloc_redecl`, `attr_excl_generic`).
- `attr_excl_generic(c, tok, AttrId an, ...)`; `cur` is `AttrId cur[24]`; `ign` is `AttrId ign[8]`; `cmerge.c:491`
  compares `AT_NOINLINE`. `attr_excl_tab` rows are `{AttrId n; AttrId ex[8]; bool fn_only}` zero-terminated by `AT_NONE`.
- Strings stay where the *source text* is the input: `attrs_item_named`, `attr_arg_eq` (called with the id's name or kept
  string-keyed: builder's call), `cdecl_attr_name`, `attr_lookup_n`.

## End-state fit

Home: `attrs.h`/`attrs.def` own ids and names; `cattr.c` owns the sets and the exclusion table; no new module.
Replaces three string stores and the `strcmp(…, "literal")` tests on them (count after the change, not estimated
here). Deletes `AName.name`, the empty-string tombstone, `cur`/`ign` string arrays, and the `attr_excl_tab` allowlist line.

## Verification

- Before any change, add `tests/check` goldens that pin the behaviours above and must not change:
  (a) a declaration whose only attribute is unknown, with two typedef sets in its type chain and a record carrying
  `packed`, queried with `__builtin_has_attribute(x, packed)` (the 3-slot case: result today is 0);
  (b) 24 unknown attributes then `pure` then `const`; (c) `copy` of a symbol whose set has a dropped entry (a typedef
  with two `aligned`); (d) a function redeclared with differing `alloc_size`.
- `tests/run.sh` 2370/0 (or the count then current) at each task commit; one full gate at the end (run/san/verify,
  callgrind recorded: AR4 stage 1 moved −0.10%).
- `sh bench/tools/attr_lists.sh` prints ok with the allowlist empty.

## Left to the builder

Task order; how `attr_arg_eq` is keyed; whether `attr_id_of` is a function or a macro; commit boundaries.

## Found, not part of this work

`e_has_attr` writes `sets[n++]` into `sets[3]` without a bound after two typedef sets plus a record set
(cexpr_bi.c:581-590): a possible overflow, reachable only with unknown-only attribute sets in a deep typedef chain.
Park it (PARKED.md) with that reading as evidence; verify under asan before fixing.

## Docs to update when it lands

PARKED.md stage-2 entry (split: stage 3 stays); ATTR_TABLE_DESIGN.md:178 ("stays allowlisted until … deleted":
the table is converted, not deleted); the allowlist's comment; `attrs.def`'s header; CLEANUP_PLAN AR4 text.

## Calls I made

Scope is j1 only (the user's pick). Unknown names keep a stringless entry (review finding: set existence is observable).
The review also found that "drop unknown, probe first" would have changed `__builtin_has_attribute` results through
the 3-slot limit, so the probe-then-drop route is gone.
