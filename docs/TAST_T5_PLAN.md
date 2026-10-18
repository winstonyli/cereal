---
status: done
---

# TAST stage T5: plan (evaluate-once temporaries)

Follows `docs/TAST_T4_PLAN.md`. The design (`docs/TAST_DESIGN.md`) deferred
temporaries to the T6 templates; this plan asks whether the structured form
needs a minimal one earlier. Facts are from the tree of 2026-10-18; anything
unread is marked **verify**. Not started: needs a go-ahead.

## 1. The problem and how big it is

`x op= y`, `++x`, `x++` read and write the same object, so the pass builds
the lvalue twice (`incdec`: `lval` at `tast.c:536-537`; `e_assign_t`: `:580`
and `:586`/`:593`). That is only right when naming the lvalue evaluates
nothing, so `twice_ok` (`tast.c:463`) refuses an lvalue whose checker node has
`EF_SIDE` and the unit becomes `unsupported: side effect in compound lvalue`.

Corpus: one unit (lua54 `ltable.c:413`, `nums[luaO_ceillog2(k)]++`). libuv
none. It is small; the reason to do it is that the refusal is a hole in a
general rule (C allows it), not that a corpus is blocked. There is no second
user: the parked overridden-initializer case looks related but does not need
it (section 5).

## 2. Design: one lvalue op, `tmp`

A new lvalue op **`tmp N`**: no kids, `val` = N (numbered from 0 in the
order created within the unit), the type is the temporary's type, printed on
the node like any other (`tmp 0 int *`); no separate table. It sits beside
`sym` in `is_lvop`: it can be stored to and loaded from; it is never static
data and no address is taken of it.

`nums[g(k)]++` becomes, in the existing nodes plus `tmp`:

    stmtexpr int
      exprstmt  store (tmp 0 : int *) (addr (deref (ptradd nums (g k))))
      post (store (deref (load (tmp 0))) (binop + (load (deref (load (tmp 0)))) 1))
                                  # the temporary's type is `int *`, the same
                                  # TypeId `addr` produces (verify: qualifiers
                                  # of the pointee are kept, `volatile int *`)

Everything but `tmp` is an existing node: the address is computed once by
the `store` into the temporary, every later mention is `deref (load tmp)`.
`stmtexpr` (T3c) already is "statements, then a value" and the verifier
already checks it, so no new control or sequencing concept.

Why a new op rather than a synthetic `local`/`sym`: `sym` and `local` carry a
checker `SymRef` and the index identity key (T1 plan, `cindex_key`); a
synthetic one would need a fake symbol that every consumer of `sym` then has
to special-case. A distinct op keeps `sym` meaning "a declared entity".

Considered and not done:
- Re-using `elvis` as a temp template: `elvis` already evaluates its left
  operand once by construction (it is one node with the value as kid), so a
  temp would add nodes, not remove any. It stays.
- `TO_POST` as a temp: `post` is a single node with a defined meaning;
  the general mechanism does not shrink it.
- Temporaries for bit-field lvalues: the address of a bit-field cannot be
  stored. A bit-field compound lvalue with a side effect stays unsupported
  (`bit-field with side effect`), the one case this cannot cover.

## 3. Where it applies

One helper `once(El*, lvalue-node) -> {setup, ref}`:

- `lval` on a node with no side effect: `setup = TN`, `ref` = the lvalue as
  today (no temp, golden output unchanged for every existing case).
- With `EF_SIDE` (and not a bit-field): allocate `tmp N : T *`,
  `setup = exprstmt (store tmp (addr lval))`, `ref` = `deref (load tmp)`,
  except a bit-field `field` lvalue: `unsupported: bit-field with side
  effect` (its address cannot be stored),
  built fresh each time it is asked for (two mentions = two `deref` nodes
  over two `load`s of the same temporary; a node is used once).

`incdec` and compound `e_assign_t` call `once` instead of `lval` twice and
`twice_ok`. If `setup != TN` the whole result is wrapped in a `stmtexpr`
(statements: `setup`; value: the original store/post). `twice_ok` is
deleted, with its comment about T6.

## 4. Verifier

- `tmp`: no kids, a pointer-typed value; all nodes with the same N have the
  same type; exactly one `store` to it.
- Every `load (tmp N)` comes after that `store` and below the same
  `stmtexpr` (the wrapper `once` builds). **Verify** how: the verifier would
  need parent links; the cheap form is index order plus the stmtexpr's
  subtree being the contiguous node range ending at it, which holds if every
  subtree is built in one run (check `incdec`/`e_assign_t` build nothing of
  the subtree outside the call that makes the wrapper).
- `sconst`/`slv` do not accept `tmp` (it is not static data).

## 5. What it does not need: the overridden initializer element

`{[0]=f(), [0]=3}` in an automatic object looked related but is not: gcc
does not evaluate the overridden `f()` (C11 6.7.9p19), so the pass drops it
like a static one. Done separately (2026-10-18), no `tmp`.

## 6. Order, checks, gates

1. `tmp` op: node, table, header print, verifier, `is_lvop`.
2. `once`, `incdec` and `e_assign_t` rewired; `twice_ok` deleted.
3. Tests `tests/tast/tmp.c`: `a[f()]++`, `(*p())--`, `s->v[g()] += 2`,
   `*f() += *g()` (one temp: only the left side is made once), nested
   (`a[b[f()]++]++`, two temps, the inner wrapper inside the outer setup),
   pointer compound (`p[f()] += 1` where `p` is a pointer), float element,
   bit-field with side effect (unsupported), side-effect free case unchanged.
4. Gates: suite; the only existing golden that changes is `unsup.c`'s
   `sideef` line (a unit that used to be unsupported); `--verify-tast` clean;
   `tests/tast_gcc.py` on `tmp.c`; coverage via `run.ps1` (lua54 4 -> 3);
   callgrind.

## 7. Risks and what is out

- Whether `EF_SIDE` also marks a `volatile` access (**verify** in
  `cexpr.c`): if so, `*vp += 1` gets a temp, which is correct (one access of
  the pointer, as C requires) and the goldens of any existing volatile case
  would change, so grep `tests/tast` for volatile first.
- The setup `store` of an address runs before the right operand of
  `x op= y`; C leaves their order unspecified, so this is allowed. The dump
  records the order the pass chose, not a promise about it.
- Out: computed goto, `asm`, array-typed parameters, the other libuv units
  (`PARKED.md`); each is its own mechanism.

## 8. Parked by this plan

- Bit-field compound lvalue with a side effect (trigger: a corpus unit).

## 9. Review log

- **Round 1 (claims against the tree).** Caught: the `lval`/`twice_ok` line
  numbers hold; the `tmp` table was redundant (the type prints on the node),
  so dropped; the test list said "two temps" for `*f() += *g()` where there
  is one; "golden diff empty" was wrong (`unsup.c` `sideef` changes); the
  second-user remark was misleading; the bit-field exclusion had no place in
  `once`.
- **Round 2 (verifier soundness).** Caught: "stored before loaded on every
  path" was hand-waved; replaced by a checkable rule (one store, loads after
  it and under the same wrapper) with the contiguity assumption marked
  verify. The type of the temporary must be the very `TypeId` `addr`
  produces or `store` fails `SAME`; noted.
- **Round 3.** Nothing material: scope (two callers, one helper, one new
  op, one deleted function) agrees across sections.

## 10. Progress (2026-10-18)

Done. The helper is three functions, `once_init`/`once_ref`/`once_wrap`
(`tast.c`), because the temporary is created once but referenced twice;
`twice_ok` is deleted. `tmp` is an lvalue op in `is_lvop`.

- Verifier: no kids, pointer type, one type per N, exactly one store per
  temp, a load only after its store (index order). The "same stmtexpr"
  containment holds by construction and is not checked (section 4's
  parent-link form was not built).
- `EF_SIDE` marks volatile accesses: `*vp += 1` gets a temp, a plain
  volatile global does not (`tmp.c` `vol`). No existing golden had a
  volatile compound case.
- Tests: `tests/tast/tmp.c` (all listed cases; only `bits` unsupported);
  `unsup.c` lost `sideef` and `ptr.expected` gained `sideeff`.
- Gates: suite 2260/0; coverage `tast-t5-coverage`: lua54 unsupported 2
  (`ADDR_LABEL` only), libuv 6, verify findings 0; data oracle lua54 107
  and libuv 32 objects, 0 failures.
- Parked: bit-field compound lvalue with a side effect (`PARKED.md`).
