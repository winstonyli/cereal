---
status: active
---

# Narrow-width Int tier and spec `*`

Closes the PARKED rows "Narrow-width index arithmetic gets no Int tier" and the spec `*` gap (`PARKED.md`, `--prove` slice 4a
leftovers). Route chosen 2026-11-21 (n1): keep narrow arithmetic in the Int tier and convert at the edges. Evidence: spike S8
(`docs/spikes/prover/README.md`): the Int encoding answers `fill2` over `int` in 0.3 s per query, bit-vectors give no answer in 90 s; spike L1:
z3's `bv2nat`/`int2bv` bridges do not help (50 s timeout); T0: the view-change fact in the hypotheses costs the same. `tests/prove` has no golden
with `*` or `+` on an `int` index, so the change reaches no existing report (2026-11-21 count).

## End-state fit

- **Where:** `ps_range` and the Int image in `prove_smt.c` (`ps_to_int`, `ps_valid_int`); `ev_arith` and the retry in `prove_vc.c`. No new
  file, no module boundary moved, one new term op (`PS_ISUB`, d1), no solver change.
- **Replaces:** `Val.ops` (the "at most three operations deep" cap) by `ps_range` (a term interval); the 64-bit-only `PS_VAR`/`PS_BVC` cases of
  `ps_to_int` by a width-and-view-aware image; the three fixed steps of `ps_valid_int` by a faithful-set fixpoint.
- **Deletes:** `Val.ops` and its five sites (`prove_vc.c:22,396,399,521,536`), the "a multiplication in a spec" refusal, the PARKED row and
  `docs/PROVE_OBLIGATIONS_DESIGN.md` section 10's pointer to it.
- **Subsumes:** `int`/`short`/`signed char`/`unsigned`/`unsigned char` leaves, sums, differences and products of them in specs and in code, in
  either view. **Cannot cover:** `*` with a 64-bit operand (refused: no exact 64-bit image); a mixed signed/unsigned 64-bit sum; truncation
  (`unknown`, not an error). Those stay `unknown` as today.

## Part A: spec arithmetic by value range

One function `ps_range(t)` in `prove_smt.c`: the structural interval of a term (a variable of `w` bits `[0, 2^w)`, `PS_RESIZE` by its flag,
a constant exact, add/sub/mul by interval arithmetic). `ev_arith` in a spec refuses when the result's range does not fit signed 64 bits
(reason "spec arithmetic that may exceed 64 bits"); a 64-bit leaf is refused as now. `ps_to_int` skips the wrap obligation of any op whose range
fits, so spec products (`r * c` over two `int`s, `2^62`) are faithful with no query. The Int tier sees terms, never `Val`, so one function
serves both sides; there is no `mag` field. `TO_CAST` of a spec value needs nothing extra (the term carries its range); negation of a spec value
is `BVSUB(0, t)` at 64 bits, exact. Subsumes the three-operation cap, admits `r*c + 1`, refuses a product with a 64-bit operand.

## Part B: the Int image of narrow terms

Bottom-up, one canonical kind per node, memoized per node (`ps_to_int` is an unmemoized walk today); fresh names are node-unique (`ps_var`
conflates equal names). An op's view is its operands' view; a **view change** happens only at the edge to a consumer or when operands disagree:
`BVULT/ULE` want unsigned, `BVSLT/SLE` signed, `PS_RESIZE` by its flag, `PS_EQ` converts one operand to the other's.
Signed `v` of width `w` to unsigned `u`: fresh Int `u` with `(v >= 0 and u = v) or (v < 0 and u = v + 2^w)` (T0 timed it at 64 bits); unsigned
to signed: `v = u - 2^w` if `u >= 2^(w-1)`, else `u`. Any width, either direction. The facts are definitional (always satisfiable) and join the
hypotheses of every query, wrap obligations included. `BVADD(0, sext idx)` needs no fold: its view is signed, its obligation trivial, and the
`BVULT` consumer gets one view change (the shape T0 measured).

- Narrow variable: one Int variable with the view's range fact (`[-2^(w-1), 2^(w-1))` or `[0, 2^w)`). Constants: Int of the value under the view;
  `ps_intc` takes `int64_t` and prints from the unsigned magnitude (`(- n)`; `INT64_MIN` cannot be negated).
- Add, sub, mul at any width: the Int op plus one obligation that the result lies in the view's range, **both bounds, any view** (an unsigned sub
  can go negative while the bit-vector result wraps); skipped when `ps_range` shows it fits. One generic obligation form for every op and view
  replaces `wrapbv`'s `a <=u ~b`: the `(w+1)`-bit (mul: `2w`) extension of the operation equals the extension of the result.
- `PS_RESIZE` widening keeps the value; truncation gives "tier does not apply".
- **Faithful set, a least fixpoint.** A hypothesis's Int image is faithful only when its own obligations hold; proving an obligation from an
  unfaithful image is circular (`x + y <= z`, `x = y = INT_MAX`, `z = 0`). Start empty plus leaf ranges and view facts; a hypothesis joins once its
  obligations are proved from the current set; repeat to no change. Hypotheses that never join, or cannot be translated (an array select), are
  dropped, which only weakens (sound). The goal's obligations are proved from the final set, then the goal over Int. Today `ps_valid_int`
  excludes every multiply hypothesis from the wrap proofs (`lin_i`), which would leave `fill2`'s `i * c` unprovable.

## Part C: wiring (`prove_vc.c`)

`ps_valid_int` has no engine caller (`prove_vc.c:837` calls `ps_valid` only; the tier runs in selftests alone). Try the Int tier first when
`ps_to_int` succeeds and a non-constant multiply is present (S8: bit-vectors give no answer in 90 s, and `ps_valid` spends RLIMIT 100M first),
otherwise on `PS_UNKNOWN`. **Take only `PS_VALID`**: an `INVALID` or error from the Int tier (a dropped hypothesis can make it `sat`) leaves the
original line. The retry uses the same `rlimit` (the tier's `rlimit * 25` could burn minutes on a scalar goal that used to stop). No `tests/prove`
golden contains "unknown" and invalid goals are not retried, so no existing report changes (grep, 2026-11-21).

## Tasks (TDD, in order; each testable alone)

0. **T0 Spike (done 2026-11-21).** Nine queries, about 0.3 s each, answers as S8. Covers signed-to-unsigned at 64 bits and a `2^62` product only;
   unsigned-to-signed and narrow widths are T2's.
1. **T1 `ps_range` and spec `*` (done 2026-11-21).** `tests/prove/spec_mul.c`/`.expected` first: `requires r * c <= $len(d)` over `int` accepted (the goal stays
   `unknown` until T3), over `unsigned long` refused with the reason line; a `selftest prove` case for `ps_range`; then delete `Val.ops`.
2. **T2 Narrow images and the fixpoint (d1 settled).** `selftest prove` cases first (a timeout is a failure, which replaces a separate guard): the S8
   goal valid; dropped `r*c <= len` invalid; dropped `r*c <= INT_MAX` unknown; the `x + y <= z` circularity case unknown; `BVULT(sext a8, c32)`
   not proved; `BVULE(BVSUB(n, i), n)` with `i <=u n` valid (replaces the "Int tier declines bvsub" case at `prove_smt.c:523`, which stays
   `unknown` without that hypothesis); unsigned-to-signed (`sext` of an unsigned-view add); nested view change; "fill2 bound without
   r <= 2^31" (`:455`) stays unknown.
3. **T3 Wiring.** Selftest that a `PS_UNKNOWN` goal becomes valid through the retry and that an `INVALID` retry is ignored; then the call.
4. **T4 `fill2` golden.** `tests/prove/fill2.c` (+ `.expected`, + one negative per dropped `requires`), the S8 shapes. Deletes the PARKED row and
   the section 10 pointer. Fixes stale text in the same commit: `prove_smt.h` "each query lives in a push/pop" (`valid_q` resets) and the
   `PsInt` comment about 8-bit values.

## Decide

- d1 **Sub in Int** (settled 2026-11-21: `PS_ISUB`): `PS_ISUB` (recommended: a multiply rewrite would count every sub as multiplied in `ps_has(t, PS_BVMUL)` and move
  it to the slow stratum) or `a + (-1) * b`.
- d2 **Range source** (settled 2026-11-21: `ps_range`): `ps_range` over terms (recommended: one function for both sides, exact for `r*c + 1`) or a `Val.mag` field
  (the first draft: needs a `TO_CAST` fix, cannot reach the Int tier, over-refuses).

## Review

Round 1 (Sonnet, 2026-11-21): circular wrap proof, multiply-hypothesis exclusion, unwired tier, `BVADD(0, ...)` goal shape, unsigned-only
`ps_intc`, narrow view mismatch, one-sided bounds, fatal untranslatable hypothesis; cut T4-as-guard and per-view variables.
Round 2 (Sonnet, 2026-11-21): `mag` cannot reach the Int tier and mishandles casts (replaced by `ps_range`); top-down views contradicted T0
(now bottom-up); unsigned-to-signed and unsigned sub missing; fresh-name collisions and no memo; stratification is a fixpoint; an `INVALID`
retry must be ignored; the bvsub selftest does not flip as first written; retry cost; cut the `x + 0` fold and the separate wrap predicates.
Size estimate: roughly 200-300 lines. Round 3 not run.

Round 3 (Sonnet, 2026-11-21): core sound (fixpoint is an induction; dropping a hypothesis only weakens; view facts always satisfiable). Folded into
T2/T3 below; T1 as built (`2051680`) needs no change (its `ps_range` uses checked builtins, refuses truncation and 64-bit leaves, and reads a
resize operand's raw width, so a wrapped narrow node under `sext` is safe).
- **Goal antecedent.** `add_goal` (`prove_vc.c:287-301`) builds `IMP(pc, g)`, so the loop test (`j < c`) is in the goal, not the hypotheses.
  `ps_valid_int` takes the top-level `IMP` antecedent conjuncts as extra hypotheses through the same fixpoint (several `IMP`s ANDed per label each
  use their own), splits top-level `AND` hypotheses (one failing conjunct must not drop the rest), and drops untranslatable ones. Without this the
  "invariant preserved" goals (`j + 1` no-wrap needs `j < c`) fall back to bit-vectors. Belongs to T2.
- **Leaf views.** `PsVar` has no signedness and `ps_var` makes a new node per call, so the Int image is per variable index `k`, with views as edge
  conversions: a leaf's image is signed when its first consumer (in term order) is signed, else unsigned. A signed default makes unsigned narrow
  `n - i` incomplete (`i <=u n`, `n = 2^31`), never unsound; a golden for it is not planned.
- **Constants.** `ps_intc` takes sign and magnitude (`2^63`, `2^64` bounds and unsigned 64-bit constants above `INT64_MAX` must stay printable).
  Fresh names come from a counter with a length check (`PsVar.name[40]`, and `ps_var` matches by name).
- **Cost.** Cache failed obligations across fixpoint rounds and goals; an Int attempt that ends `unknown` spends a full `rlimit` before the
  bit-vector retry spends another, so the Int-first order applies only when a non-constant multiply is present.
