---
status: done
---

# TAST stage T7: plan (compound literals)

Follows `docs/TAST_T6_PLAN.md`. Facts are from the tree of 2026-10-18; anything
unread is marked **verify**. Built 2026-10-18 (section 10).

## 1. The problem and how big it is

A compound literal `(T){...}` is an unnamed object with an initializer. The
checker types it as an lvalue (`e_complit`, `cexpr.c:5068`: `EF_LVALUE`; at
file scope also `EF_ADDRLV`, an address constant). The pass has no case for
`N_COMPOUND_LIT`, so `lval` falls to "lvalue form" (`tast.c`, `lval` default).
`PARKED.md` has two rows: block scope and file scope.

Corpus: one unit, libuv `linux.c:1856`, `(*ci)[i++] = (uv_cpu_info_t){..}`
(block scope, nested literal for `.cpu_times`). lua54 none. File scope: no
corpus unit. The reason to do it is the one libuv unit, and that C uses the
form freely (`&(T){..}` arguments, struct returns, designated defaults).

## 2. Design: one lvalue op, `complit`

A new lvalue op **`complit N`**: kids are the initializer, exactly as `local`
has them (`elem` nodes, or one value for a scalar), `val` = N (numbered from 0
within the unit, like `tmp`), `pun` = 1 for static storage (file scope), 0 for
automatic; the type is the literal's type (a complete type; the checker has
already sized `(int[]){1,2,3}`). It is in `is_lvop`: it can be loaded from,
have its address taken, be a `field` base or `ptradd` base (after `addr`).

    (*ci)[i++] = (T){ .a = 1, .n = (N){ .u = 2 } }
      store (deref (ptradd .. (post ...))) (load (complit 0 #elems T))
    # the inner literal is a second `complit 1` under an `elem` of the first

The object is created where the node is evaluated: elements run in order, the
node then designates the object. One node does both initialization and naming.

Why this and not a `local` plus `tmp` (the T5 shape): `lval` returns one
lvalue node, and the declaration would have to come before it as a statement.
`stmtexpr` yields a value, not an lvalue, so `&(T){..}` and `(T){..}.f` would
need a pointer temporary (`tmp`, T5) wrapped around every use, and hoisting the
declaration out to the statement breaks the order of `f(g(), (T){h()})`.
`complit` keeps the order for free: it is a leaf of the expression tree.

Considered and not done:
- Generalizing `tmp` to any unnamed object: `tmp` is declared by its store
  (T5 plan section 2); an unnamed object with an initializer needs a
  declaration form too, which is a second node anyway, plus it changes the
  T5 goldens. `complit` stays one node.
- A synthetic `CSym` per literal: T5 section 2 (every consumer of `sym`
  would special-case a symbol without a declaration).

## 3. Where it applies

- `lval`: `case N_COMPOUND_LIT`: the type child `k[0]`, the initializer list
  `k[1]`. Build the kids with `init_elems(e, k[1], ty, is_static, ...)` (the
  same function declarations use, so designators, overridden elements,
  strings and nested literals all come for free); a scalar literal
  `(int){5}` is a one-element list **verify** `init_elems` accepts a scalar
  with braces.
- `ex`: nothing to add: `ef & EF_LVALUE` already routes through
  `load(lval)`, and arrays decay through `addr_t(lval)` (so `(int[]){1,2}`
  as a pointer works).
- Static vs automatic: `e->infunc` (file scope = static, which is also what
  the checker uses for `EF_ADDRLV`).
- `sizeof (T){..}` and `typeof`: folded by the checker, never reach `lval`.
- A VLA literal: refused by `ok_ty` ("array type"), unchanged.

## 4. Verifier

- `complit`: kids are `elem`s (as `local`; `velems(e, out, i, is_static)`
  with `is_static` = `pun`) or one value of the same type; a static one is
  checked by `velems` to hold only constant elements (`sconst`).
- `is_lvop`: `complit` is an lvalue; a value position must not hold it bare
  (the existing "values are never a bare lvalue" rule).
- `sconst`/`slv`: `slv(complit)` is true when `pun` is 1, so `&(T){..}` in
  static data is a link-time constant. Automatic: false.
- Numbering: N distinct within the unit; unlike `tmp` there is no store
  rule.

## 5. File-scope literals and emission

`int *p = (int[]){1,2};` is a `data p` unit whose elem holds `addr (complit 0
..)`. The anonymous static object is part of that unit's tree, not a separate
unit, so a consumer emitting data sees `complit` with `pun` = 1 under an
`addr` and emits the object there. No second unit, no cross-unit numbering.
**Verify** `velems`' "static initializer is not constant" accepts an
`addr (complit)` whose own kids are constant (it recurses into `sconst`;
`sconst` needs to look into the `complit` kids: add them).

## 6. Order, checks, gates

1. `complit` op: node, table, print (`complit N` with kids), `is_lvop`,
   verifier, `slv`/`sconst`.
2. `lval` case; `init_elems` called with the literal's list and storage.
3. Tests `tests/tast/complit.c`: struct literal assigned; `&(T){..}` argument;
   `(int[]){1,2,3}` decaying; `(T){..}.f`; nested literal; scalar `(int){5}`;
   `(char[]){"ab"}`; automatic literal with side effect in an element;
   file-scope `int *p = (int[]){1,2}` and a struct literal address; the
   evaluation-order case `f(g(), (T){h()})`.
4. Gates: suite; `--verify-tast`; coverage via `run.ps1` (libuv 3 -> 2);
   data oracle on file-scope cases (**verify** gcc names the object
   `.compoundliteral`/`.LC` or similar; compare by position and bytes, not
   name; skip if not comparable, as for `labaddr`); gate.

## 7. Risks and what is out

- Lifetime: an automatic literal lives to the end of the enclosing block; the
  pass records the creation point only. A loop re-creates it each iteration
  (correct: each evaluation is a new object). No verifier rule.
- `goto` past a literal's creation point leaves it uninitialized, as for a
  declaration; no rule.
- Out: VLA literals, C++-style literals, the other libuv units (`PARKED.md`).

## 8. Parked by this plan

None expected; the two `PARKED.md` rows close when built.

## 9. Review log

- **Round 1 (design).** Caught while drafting: the first design reused T5's
  `stmtexpr` + `tmp`, which cannot return an lvalue, so `&(T){..}` and
  `(T){..}.f` would not fit and a hoisted declaration breaks evaluation
  order; replaced by the single `complit` lvalue op.
- **Round 2 (against the tree).** Caught: `ex` needs no change (the literal is
  already `EF_LVALUE`); `sconst` must see through `complit` kids for a static
  literal (section 5); the checker, not the pass, sizes `(int[]){..}`.
- **Round 3.** Nothing material beyond the **verify** items (scalar braces in
  `init_elems`, gcc's object naming), left to the build.

## 10. Progress (2026-10-18)

Done as planned: `complit` (lvalue op, `pun` = static), `lval` case calling
`init_elems`, `slv` true for a static one, verifier case via `velems`.
`tests/tast/complit.c`; `unsup.c` lost `cl`. Suite 2268/0. Coverage
`tast-complit-coverage`: lua54 0, libuv 2 (`asm`, `setproctitle`, both
decided against), verify findings 0. Data oracle: lua54 107 (2 skipped,
`labaddr`), libuv 34, 0 failures.

- One checker change: `cinit.c` kept no node for an element that is itself a
  compound literal (`r.node = v->cl ? NOB : v->node`); it now always keeps
  it, so `.n = (N){..}` is a `load` of a nested `complit`.
- Section 5's **verify** resolved: `slv(complit)` is true when static and
  `velems` on the literal checks its elements constant, so no `sconst`
  change was needed.
- Added: a static object initialized by a compound literal value
  (`static T x = (T){..}`) is `unsupported: static initializer from a
  compound literal` (a `load` is not constant); not in the corpus.
- Scalar braces `(int){5}` work.
- **Data oracle extended (same day).** `tests/tast_data.py` treats a static
  `complit` as gcc's `__compound_literal.N` object and an `addr` of it as a
  relocation to it; `tests/tast/cldata.c` (9 literals: pointers, nested,
  string, offset, indexed) matches gcc: 13 objects, 0 failures. It found a
  silent error: a static aggregate member initialized by a literal *value*
  (`struct Q q = { (struct A){2} }`) dumped as an empty `data` node, because
  the checker folds the literal's elements under the literal's own list.
  Fixed by building the fold (later the same day): `cinit.c` `tin_fold`
  copies the literal's records into the enclosing list at the member's
  offset (a copied `TI_CLEAR` keeps its fence relation), and `init_elems`
  takes a top-level static literal value as its own list. `cldata.c` holds
  the cases (plain, overridden whole, overridden in part, array of them, a
  static local): 19 objects match gcc, 0 failures. An array literal that
  decays to a pointer is unchanged.
