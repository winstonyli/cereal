---
status: done
---

# Pure/const state in the name sets (AR4 stage 3)

Follows `docs/ATTR_IDS_DESIGN.md` (stage 2, done). Scope chosen 2026-11-05: pure/const only; the other `CSF_*` attribute
mirrors and the `Attrs` bools stay (evidence below). Plan: `docs/PURE_CONST_PLAN.md` (active from Task 1; the gate's `san`
stage is the asan run). Shape: prototype `proto-y1` (2026-11-06; session scratchpad, not committed): the suite passes except
the two `attr_copy_excl` hunks below, and its probe lines match gcc 15.2 except the function `common, nocommon` gap,
`has_attribute` on variables, and gaps present on `main` (see Behaviour changes). An earlier variant with a per-pair note
rule (`proto-x1`) passed the gate (run `2026-11-06-ar4s3-proto-x1`) but encoded a note rule gcc does not have.

## Problem

"This function is pure / const" is held three times:

1. `AttrState` (`cdecl_int.h`): `pure`, `cnst`, `pure_from`, `cnst_from`, `pure_loc`, `cnst_loc`, used by `attr_excl` and
   `attrs_state_init` (`cattr.c`) only.
2. `CSym.flags`: `CSF_PURE`, `CSF_CONSTFN`, written from (1) at `cdecl.c:2411` and `cdecl.c:3361`, read by
   `attrs_state_init`, the function branch of `attrs_copy_check`, `cmerge.c:725` (merge mask) and `cwarn_expr.c:912`
   (call equality).
3. The `AT_PURE` / `AT_CONST` entries of `CSym.aset` (built by `cdecl_attrs_names`, carried across redeclarations by
   `attrs_names` at `cdecl.c:2644-2646` and replaced per declaration at `cmerge.c:748`), which hold the same fact.
   `ccall.c:1624-1626` already derives pure/const from the set; call equality is the one reader still on the flags.

The pair's conflict check is also done twice: `attr_excl_generic` skips the pure/const pair (`continue`, "attr_excl") and
`attr_excl` (about 25 lines) redoes it with its own state. Separately, `attr_excl_tab` carries `fn_only` to keep four
rows off variables, but the other rows still fire on variables and print conflicts gcc does not (below).

## Design

Place: exclusion state and rules in `cattr.c` (`attr_excl_tab`, `attr_excl_generic`, `attrs_copy_check`) and `AttrState`
in `cdecl_int.h`; the flag enumerators in `check_int.h`; their writers in `cdecl.c` (two lines) and the merge mask in
`cmerge.c`; the one pure/const predicate beside `cdecl_aset_has`, used by `ccall.c` and `cwarn_expr.c`; the guard in
`bench/tools`. No new module.

1. **Gate by declaration kind.** `attr_excl_tab`'s `fn_only` becomes `obj`: a row applies to a declaration when
   `obj ? kind == CS_OBJ : kind == CS_FUNC`. Only the two `common`/`nocommon` rows are `obj`. gcc drops an inapplicable
   attribute ("attribute ignored", already printed elsewhere) before it can conflict, so exclusion never applies to
   variables. Today we print a conflict as well for `noreturn,pure`, `const,noreturn`, `malloc,pure`, `cold,hot`,
   `noinline,always_inline`, `returns_twice,noreturn` on a variable; gcc 15.2 prints none (checked 2026-11-06). The
   pure/const `continue` guard is then unnecessary and deleted: the pair is a function-only row like the rest.
2. **One note rule.** `attr_excl_generic` already notes the *incoming* attribute's origin: the previous declaration
   (`st->prevloc`) when the conflict is with it, else the `copy` source (`copy_set`/`copy_loc`), and nothing for an
   attribute written on this declaration. That is gcc's rule for every pair, including pure/const (gcc 13.4 and 15.2
   agree on 20 probe lines, reviewer run 2026-11-06). Only `attr_excl` differed, noting the *kept* attribute's origin.
   Going generic needs no per-entry origin: `AttrState.cur[]` stays `AttrId[24]`.
3. **Copy.** The function branch of `attrs_copy_check` is the object branch with a different id pair: one loop over a
   per-kind pair (function: `AT_PURE`, `AT_CONST`; object: `AT_COMMON`, `AT_NOCOMMON`) using
   `cdecl_aset_has(c, r->aset, id, NULL)`, `copy_set`/`copy_loc` and `attr_excl_generic`; `st->nonnull |= r->nonnull`
   stays for functions. Previous-declaration pure/const is found by `attr_excl_generic` through `st->pset`.
4. **State.** `AttrState` loses its six fields; `attr_excl` and its call branch, and `attrs_state_init`'s flag seeding,
   are deleted.
5. **Flags.** `CSF_PURE` and `CSF_CONSTFN` are deleted: enumerators, the two writes, the merge mask bits. Call equality
   asks a new predicate `cdecl_fn_pure(c, ref)` (`csym(c, ref)->kind == CS_FUNC` and the set has `AT_PURE` or
   `AT_CONST`), which replaces only the set test open-coded at `ccall.c:1624-1626`. The early `return false` for a non-function at
   `ccall.c:1622` stays (it also skips the built-in table lookup, so a variable named like a pure built-in is not pure).
   The kind test in the predicate is needed: the flags were written for functions only, but a variable's set
   records `pure` too (`int (*fp)(int) __attribute__((pure));` has it), and `fp(x) == fp(x)` must stay silent. Other
   enumerator values keep their numbers.
6. **Guard.** `bench/tools/attr_lists.sh` gains a second check (a new parse of `check_int.h`, not the string-array scan):
   a `CSF_*` enumerator whose comment names `__attribute__((x))` with `x` a row of `attrs.def` fails unless
   `bench/tools/attr_lists.allow` lists it as `check_int.h CSF_X <reason>` (the existing "FILE ARRAY" format; the
   reader ignores the trailing reason). The new hits are appended to the script's one `$T.hits` file, so the existing
   `comm` serves both checks: a new hit not in the list fails, a listed entry that no longer matches is reported stale,
   and the list only shrinks. The header of `attr_lists.allow` is updated (it says "empty since AR4 stage 2"). Limit: a
   mirror whose comment does not name `__attribute__((x))` is invisible to the check (only single-line `CSF_X =`
   enumerators are parsed; a `#define` or a multi-line comment, like `CSF_CONST_VAL`, is invisible). The allowlist starts with the
   seven kept mirrors:
   `CSF_NORETURN` (its comment gains exactly `/* _Noreturn or __attribute__((noreturn)) */` so the check sees it), `CSF_WEAK`,
   `CSF_ATTR_UNUSED`, `CSF_DEPRECATED`, `CSF_UNAVAILABLE`, `CSF_GNU_INLINE`, `CSF_WEAKREF`.
7. **Comment.** The stale block comment above `attr_excl_tab` ("'pure' and 'const' exclude each other, and 'copy' ...")
   is rewritten to describe the table and the kind gate.

## Behaviour changes (toward gcc 15.2 except the one flagged; each pinned by a new golden unless said)

- The non-`obj` rows stop firing on variables (gcc prints none there; `aligned/packed` and `stack_protect`
  already behaved so). `attr_excl_variable` has one case per such row: the six named above, plus alloc_size/alloc_align
  with const|pure|noreturn, warn_unused_result with noreturn, const with malloc, gnu_inline with noinline, noipa with
  always_inline, and the unchanged aligned/packed and stack_protect pairs as pins.
- **Away from gcc (known, not pinned):** an attribute gcc drops as inapplicable to a variable now stays in the variable's
  set, because `c->ign` was only written by the conflict. `int v __attribute__((noreturn, pure))`: gcc and `main` say
  `__builtin_has_attribute(v, pure)` is false; the prototype says true. `main` already differs from gcc on other
  inapplicable attributes there (`noreturn`, `cold`, `const` alone). The general fix is to drop attributes inapplicable to
  the declaration kind from the set; parked as a separate item.
- **Away from gcc, accepted 2026-11-06 (decision z1; not pinned):** the kind gate treats a function-pointer object as a
  variable. gcc applies `noreturn`/`const`/`pure` there to the pointed-to function type, so it still reports
  `int (*fp)(int) __attribute__((noreturn, pure))` (conflict) and `void (*p)(void) __attribute__((const, noreturn))`;
  `main` matches gcc, the prototype prints nothing. (`pure`/`const` on a function-pointer variable also prints no
  "attribute ignored" where gcc does; the call-equality golden pins the silence of `fp(x) == fp(x)`, gcc agreeing, and
  records that exception: gcc prints "ignored" there, cereal nothing.) For `cold,hot`, `noinline,always_inline`, `returns_twice,noreturn`,
  `malloc,pure`, `gnu_inline,noinline` on a function pointer gcc prints only "attribute ignored", while `main` prints a
  conflict plus another warning, so the gate would improve those. Unpinned by any listed golden.
- **Away from gcc (new, not pinned):** `copy(fp)` of a function-pointer variable into a function carries `pure` into the
  function's set through `aset_copy`, so call equality warns where gcc is silent. A `pure`/`const` conflict past the
  24-attribute `cur[]` cap is lost (`attr_excl` tracked the pair outside `cur[]`). Both parked.
- Copy now enters the exclusion state, so `has_attribute` follows gcc on `copy(p), copy(q)` (const false), `copy(p), const`
  (const false), `const, copy(p)` (pure false), and on `copy(t)` of a `pure, const` function (const false); the old note
  at `p` for `copy(p), const` is gone (gcc prints none). A redeclaration `pure` then `const` makes `const` false in the
  set. `pure, const, noreturn` reports `noreturn` against `pure` (gcc; `main` said `const`).
- `int w0(int) __attribute__((common, nocommon));` (a function): gcc 15.2 prints "'common' attribute ignored" and
  "'nocommon' attribute ignored"; we print one wrong conflict. The gate drops the conflict, and the prototype then prints
  nothing: the ignored-attribute path had no function context. Fixed 2026-11-06 (after v0.30.0): `AC_FN` in `attrs.def` `ctx`;
  pinned by `tests/check/attr_common_function`.
- Copy now goes through the whole table: `__attribute__((noreturn, copy(p))) int f(int);` with `p` pure warns "ignoring
  attribute 'pure' because it conflicts with attribute 'noreturn'" with a note at `p`, and `copy(p), noreturn` ignores
  `noreturn` (gcc 15.2 does both; cereal printed nothing).
- `int f(void) __attribute__((pure, const));`: `__builtin_has_attribute(f, const)` is now false (the ignored `const` is
  recorded in `c->ign`, as every other excluded attribute, instead of staying in the set).
- `_Noreturn int f(void) __attribute__((const));` then `int f(void) __attribute__((pure));`: the conflict is now reported
  with `const` (table order, the pair is no longer special) instead of `noreturn`.
- Copy notes follow gcc's rule for pure/const (`attr_copy_excl.expected` changes by two hunks, verified against gcc 15.2
  on the unmodified test): line 11 `copy(a), copy(b)` notes `b` (4:27), not `a`; line 12 `pure, copy(a)` gains the note at
  `a` (3:28). `const, copy(p)` now notes `p`, closing the old gap.

Not in scope (behaviour extension, parked): copying `hot`/`cold`/`noreturn`/`returns_twice`/`alloc_*` through `copy` so
that they conflict as gcc does. cereal copies only pure/const/common/nocommon today.

## Goldens (each new file adds 2 test passes)

1. `attr_excl_variable`: one variable case per exclusion row (list under Behaviour changes); expected output only
   "attribute ignored" warnings, as gcc 15.2. (`common, nocommon` on a typedef/field/parameter pins nothing new: those
   kinds never reached the table.)
2. `attr_pure_const_has`: `has_attribute` for `pure, const` (const false, pure true), after a redeclaration, and the
   `copy` cases listed under Behaviour changes. Only cases where gcc agrees.
3. `attr_pure_const_noreturn`: `_Noreturn` + const then pure redeclaration, and `pure, const, noreturn`. (The
   `_Noreturn ... const` redeclaration case was a false conflict, fixed 2026-11-06: see "Follow-up".)
4. `attr_pure_const_copy`: `copy(a)` then `const` / `pure` on the declaration, `const, copy(p)`, `noreturn, copy(p)` and
   `copy(p), noreturn` (pins the merged copy loop), and copying a function whose own `pure` was excluded.
5. `attr_pure_const_call`: calls with equal arguments (the equality warning needs the fact) after a redeclaration
   without the attribute, after a definition, after `copy`, and with `pure, const` (const dropped);
   and silence for a function-pointer variable carrying `pure` (pins the `CS_FUNC` test).
Existing goldens that cover the rest: `attr_excl`, `attr_copy_excl`, `attr_pure_const_void`, `misc_105`.

## Verification

Per-golden mutation (each reverted with `touch` + sync; plan Tasks 1-3): make the non-`obj` rows apply to `CS_OBJ` again
(`attr_excl_variable`); drop the `kind == CS_FUNC` test in the predicate, then each of its `AT_PURE` / `AT_CONST` halves
(`attr_pure_const_call`); re-add the pair `continue` (`attr_copy_excl`, `attr_pure_const_has`); make the copy loop skip
the function pair or drop its `AT_CONST` entry (`attr_pure_const_copy`); drop `st->nonnull |= r->nonnull` (`attr_copy_excl`
line 27); drop the `copy_set` note branch (`attr_copy_excl` lines 11-12); re-add a `CSF_PURE` enumerator with a
`__attribute__((pure))` comment (the guard). The `obj` rows applied to functions too survive by design: that half
(`common`/`nocommon` on a function) was unpinned until the 2026-11-06 `AC_FN` fix. The suite passes unchanged
apart from the new files and the two `attr_copy_excl.expected` hunks; asan is the gate's `san` stage; gate green. Callgrind:
the gate prints `Collected`; compare with run `ar4s2-final2` (the aset-chain walk in call equality is the expected cost).

## Found, kept (evidence, 2026-11-05)

- The seven kept flags are not plain mirrors: `CSF_NORETURN` also comes from `_Noreturn` (`cdecl.c:1330-1334`) and feeds
  `SF_NORETURN` (`csum.c:272`); `CSF_DEPRECATED`/`CSF_UNAVAILABLE` use an exclusive encoding (`unavailable` wins,
  `cdecl.c:2483`), are also stored in `dep` on records, fields and enumerators (`crecord.c:773`, `1066`, `1354`) and pair
  with `dep_msg`; `CSF_WEAKREF` implies `CSF_WEAK` (`cdecl.c:2497`). Folding them would add a second encoding.
- `Attrs` bools and `aset` entries are not a synced pair: `aset` is built from attribute nodes by `cdecl_attrs_names`,
  `Attrs` by `attr_collect`; `attr_ids_tombstone` pins that they differ (written versus kept).

## Follow-up (2026-11-06, after v0.30.0)

Two parked false positives, each probed against gcc 15.2, fixed with goldens written first (2390 -> 2392 passed):

- Call equality (`f(x) == f(x)`) warned for a `pure` callee; gcc warns only for `const` (a pure call is not a
  tautology; `ccall.c`'s no-side-effects test still takes either). `cdecl_fn_pure(c, ref)` became
  `cdecl_fn_has(c, ref, id)`; `ccall.c` asks pure or const, `cwarn_expr.c` asks const. Golden `attr_pure_const_call`.
- The `_Noreturn` keyword added `noreturn` to the symbol's set like the attribute, so a later declaration's `pure`,
  `const` or `malloc` conflicted with it; gcc keeps the keyword out of the attribute list (so no conflict, and `malloc`
  gets the return-type warning instead), while `__builtin_has_attribute(f, noreturn)` is still true. The keyword's
  entry now carries the argument `ATTR_ARG_KEYWORD` and the exclusion checks (`aset_written`, the `malloc` check)
  skip it. Golden `attr_noreturn_keyword_redecl`.
- One attribute loop (2026-11-07, user "q2-2", s1 c1 t1 v1). `attrs_alloc_check` (handlers) and `attrs_copy_check`
  (exclusion) were two passes over the attributes, so a handler warned for an attribute the exclusion then dropped.
  `attrs_apply_decl(c, holder, &AttrDecl, st)` now runs, per attribute in source order, `attr_excl_generic`, then
  `attr_handle` (the old handler chain), then `attr_ctx_item` (the ignored-in-this-context check, shared with
  `attrs_ctx_check1` for fields and parameters); one walk over `sn`, the `*` chain and `idecl` serves functions, objects
  and typedefs. Deleted: `seen_pure`/`seen_const`, the local `malloc` exclusion, the typedef and ctx-check blocks in
  `cdecl.c`. Golden `attr_apply_order`. The pairwise probe went from 318 to 209 differing lines; the function-pointer
  variable family (about 163) was untouched (c1), as are `alloc_name`/`alloc_via_ptr`, which the `acc_*` code also uses.
- Function-pointer variables (2026-11-07, user "y1"): `AC_FPV` (a row bit in `attrs.def` `ctx`) makes `pure`, `cold`,
  `hot`, `always_inline`, `returns_twice` and `nothrow` print "'X' attribute ignored" on a pointer-to-function object
  (`attr_ctx_item`; the old function-type early return skipped it); `AF_FPKEPT` (`const`, `noreturn`) marks the two
  that stay on such a variable, so `attr_excl_generic` lets only those conflict there. Golden `attr_fnptr_ignored`; the
  probe went 209 -> 56 differing lines (`fnptr-var` 163 -> 10, left: `noinline` order, parked). `attr_pure_const_call`'s
  golden gained the "pure ignored" line gcc prints for its `fp` declaration.

## End-state fit

- Target: pure/const state lives only in `CSym.aset`; exclusion lives only in `attr_excl_tab` + `attr_excl_generic`.
- Replaces: `AttrState` pure/cnst fields, `attr_excl`, `CSF_PURE`/`CSF_CONSTFN`, `fn_only`, the pair `continue`, the
  function/object split in `attrs_copy_check`.
- Deletes: `attr_excl`, six state fields, two flags, the pair `continue`, one copy branch (prototype `proto-y1`: +17/-60
  over six files, before goldens and the guard; as built, tests and guard included: 8 files, +45/-65 in the Task 3 commit).
- Adds: the `obj` gate (replaces a column), `cdecl_fn_pure` (one set test, one flag test removed from call equality), one
  guard check. No new fields.
- Special cases that remain: the noipa/noreturn oddities already in `attr_excl_generic`; none added.

## Non-goals

The seven kept flags, `Attrs`, handler dispatch, `common`/`nocommon` on typedefs and fields (see Risks).

## Risks

- **`common`/`nocommon` kinds.** Only `CS_FUNC` and `CS_OBJ` reach `attrs_copy_check` (`cdecl.c:2391`), so typedef,
  field and parameter never reached the table. The one kind that changes is `CS_FUNC` (above). Local and file-scope
  objects of non-function-pointer type print the same as gcc 15.2 on `main` and stay so; function-pointer objects are
  the open regression above.
- **Golden edit.** `attr_copy_excl.expected` was generated by cereal, not gcc, on the two hunks above; the edit is
  checked against gcc 15.2 output of the same file.

## Docs to update

Done with the plan's Task 4: `PARKED.md` (stage-3 row deleted, section renamed, the eleven gap entries under "Attribute
exclusion gaps left by AR4 stage 3"), `docs/CLEANUP_PLAN.md` (AR4 line), `docs/STATUS.md` (golden count),
`docs/ATTR_TABLE_DESIGN.md` ("Guard": the second check), `docs/ATTR_IDS_DESIGN.md` and `docs/ATTR_IDS_PLAN.md` (the
`fn_only` spelling; stage-2 claims marked as such), `bench/tools/attr_lists.allow` header and the
`bench/tools/attr_lists.sh` header comment.
