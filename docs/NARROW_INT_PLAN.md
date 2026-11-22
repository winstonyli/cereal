---
status: draft
---

# Narrow-width Int tier and spec `*`

Closes the PARKED rows "Narrow-width index arithmetic gets no Int tier" and the spec `*` gap (`PARKED.md`, `--prove` slice 4a
leftovers). Route chosen 2026-11-21 (n1): keep narrow arithmetic in the Int tier and convert at the edges. Evidence: spike S8
(`docs/spikes/prover/README.md`): the Int encoding answers `fill2` over `int` in 0.3 s per query, bit-vectors give no answer in 90 s; spike L1:
z3's `bv2nat`/`int2bv` bridges do not help (50 s timeout). `tests/prove` has no golden with `*` or `+` on an `int` index, so the change
reaches no existing report (2026-11-21 count).

## End-state fit

- **Where:** part A in `prove_vc.c` (`ev_arith`, `Val`); part B in `prove_smt.c` (`ps_to_int`, `ps_valid_int`). No new file, no new module
  boundary, no new term op, no solver change.
- **Replaces:** `Val.ops` (the "at most three operations deep" cap) by `Val.mag` (a bit-magnitude bound); the 64-bit-only `PS_VAR`/`PS_BVC`
  cases of `ps_to_int` by a width-and-view-aware image.
- **Deletes:** the `a.ops + b.ops + 1 > 3` rule and the "a multiplication in a spec" refusal; the PARKED row (and `docs/PROVE_OBLIGATIONS_DESIGN.md`
  section 10's pointer to it).
- **Cases it subsumes:** `int`/`short`/`signed char`/`unsigned`/`unsigned char` leaves, products and sums of them in specs and in code, in either
  view. **Cannot cover:** `*` on 64-bit operands (kept refused: a product of two 64-bit values has no 64-bit exact image); a 64-bit sign-extended
  value inside a sum with a 64-bit pointer offset (no `ite` in the term set; the tier says "does not apply", so the result stays `unknown`, as today).

## Part A: spec arithmetic by magnitude (`prove_vc.c`)

`ops` becomes `mag`: a value `v` has `|v| <= 2^mag`. A signed narrow leaf has `mag = w - 1`, an unsigned one `mag = w`; `a ± b` has
`max(a.mag, b.mag) + 1`; `a * b` has `a.mag + b.mag`. A result with `mag > 62` is refused (reason: "spec arithmetic that may exceed 64
bits"); a 64-bit leaf (not a spec result) is refused as now. The value is `ext64` of both operands then the 64-bit op, exact because
`mag <= 62` fits signed 64. Subsumes the three-operation cap (`32 + 3 = 35`) and admits `r * c` for two `int`s (`31 + 31 = 62`);
`unsigned * int` (63) and anything with a 64-bit operand stay refused.

Set `mag` in `mkv` for every `Val` (`w - sgn`), so "leaf" versus "spec result" needs no separate test: `ops == 0` disappears from `ev_arith`,
`TO_CAST` and `TO_UNOP` (a spec value stays exact in the bit-vector tier; only the Int tier is ever invoked for the nonlinear goals).

## Part B: the Int image of narrow terms (`prove_smt.c`)

One mechanism, **view change**. Every Int image is of one canonical kind per node: the signed value of a signed-interpreted node, the unsigned
value otherwise (chosen top-down: `PS_RESIZE` by its flag, `BVSLT/SLE` operands signed, `BVULT/ULE` unsigned; a mismatch at any width, e.g.
`BVULT(sext a8, c32)`, is a view change). A view change from signed `v` of width `w` to unsigned makes a fresh Int `u` with the definitional
fact `(v >= 0 and u = v) or (v < 0 and u = v + 2^w)` added to the hypotheses (S8 measured this disjunction in the goal; it must be re-timed in
the hypotheses: spike first, see T0). This subsumes the per-view variables, the compare-site split and the `BVADD(0, sext i)` shape in one rule.

- Narrow variable: one Int variable, range fact `[-2^(w-1), 2^(w-1))` or `[0, 2^w)` by view, into `obl->nonneg`.
- Constants: Int of the value under the view; `ps_intc` takes `int64_t` and prints `(- n)` (negative literals are new).
- Add/sub/mul at width `w`: the Int op (new `PS_ISUB`, Decide d1) plus a range obligation for the result, **both bounds** for signed
  (`[-2^(w-1), 2^(w-1))`) and for the 64-bit signed view (`[-2^63, 2^63)`).
- `PS_RESIZE`: widening keeps the value (view follows the flag); truncation is refused.
- **Stratified obligations (fixes a circularity).** A hypothesis's Int image is faithful only once its own ops are proved not to wrap, so
  proving a wrap obligation from unproved images is circular (`x + y <= z`, `x = y = INT_MAX`, `z = 0`). Order: (1) for every hypothesis and the goal,
  adds with no multiply under them are proved in bit-vectors (`wrapbv`: unsigned `a <=u ~b`; signed needs a signed-overflow predicate,
  `a >= 0 -> b <= INT_MAX - a` and the mirror, not the unsigned one); (2) multiplies and sums over them are proved in Int from only the
  hypotheses already proved faithful; spec products are faithful by `mag`; (3) the goal over Int. `ps_valid_int` currently excludes every
  multiply hypothesis from the wrap proofs (`lin_i`), which leaves `fill2`'s `i * c` unprovable: the multiply hypotheses join once their own
  obligations pass.
- An untranslatable hypothesis (an array select) is dropped, not fatal (sound: weaker); a goal that cannot be translated stays `unknown`.

## Part C: wiring (`prove_vc.c`)

`ps_valid_int` has no caller in the engine (`prove_vc.c:837` calls `ps_valid` only; the Int tier runs in selftests alone). On
`PS_UNKNOWN` from `ps_valid`, retry once with `ps_valid_int`; a `valid` there is `valid`. Fold `x + 0` in `TO_PTRADD`/`access` so the access goal
is `BVULT(sext idx, len)` rather than `BVULT(BVADD(0, sext idx), len)`. Spec hypotheses are `BVULE(BVMUL64(sext r, sext c), len)`
with `sg = false` from the unsigned `len`; Part B's view change covers it.

## Tasks (TDD, in order)

0. **T0 Spike (done 2026-11-21: 9 queries, ~0.3 s each, answers as S8; docs/spikes/prover/README.md).** Time the S8 `full` and `no_rcl` queries with the view-change fact in the hypotheses instead of a goal disjunction. If it
   does not answer in about a second, stop and report; the design falls back to the S8 goal-site split for the compare operand only.
1. **T1 Spec magnitude.** `tests/prove/spec_mul.c`/`.expected` first: `requires r * c <= $len(d)` over `int` accepted, over `unsigned long` refused
   with the reason line; `selftest prove` case; drop `ops` as above.
2. **T2 Narrow images.** `selftest prove` cases first (they also fail on a timeout, which replaces a separate guard): the S8 goal valid; dropped
   `r*c <= len` invalid; dropped `r*c <= INT_MAX` unknown; the `x + y <= z` circularity case unknown; `BVULT(sext a8, c32)` not proved; mixed
   views of one variable. Update "Int tier declines bvsub" (it flips) and keep `x < x + 1` unknown.
3. **T3 Wiring and `fill2` golden.** `tests/prove/fill2.c` (+ `.expected`, + one negative per dropped `requires`), the S8 file shapes. Deletes the
   PARKED row and the pointer in `docs/PROVE_OBLIGATIONS_DESIGN.md` section 10.

## Decide

- d1 **Sub in Int** (settle before T2): add `PS_ISUB` (recommended: one op) or rewrite `a - b` as `a + (-1) * b` (no new op; the constant
  multiply is linear, so the cost is one more node, not solver speed).

## Review

Round 1 (Sonnet, cut-only and soundness, 2026-11-21): caught a circular wrap proof, the multiply-hypothesis exclusion that blocks `fill2`, the
unwired tier, the `BVADD(0, ...)` goal shape, the unsigned-only `ps_intc`, a view mismatch at narrow widths, one-sided 64-bit bounds, the
fatal untranslatable hypothesis; cut T4 and the per-view variables. Size estimate rose from 60-100 to roughly 150-250 lines. Round 2 not yet run.
