---
status: draft
---

# AR3: one integer-op kernel and one canonicalizer

Item AR3 of `docs/CLEANUP_PLAN.md`; scope chosen 2026-10-31: all four sites (cexpr, eval, ppexpr, pcexpr), staged. Follows AR2 (`docs/AR2_DESIGN.md`, done).

## Problem

Four hand-written copies compute `+ - * / % & | ^ << >>` in two's complement on up to 64 bits, with the policy for overflow, division by zero and shift ranges woven into each:

| Site | Where | Width | Reports through |
|---|---|---|---|
| constant folding | `cexpr.c:174 int_op` (2 callers: `:6245` shifts only, its flags unused; `:6525` the other operators, `ovf` goes to `bin_value`) | type's bits | `*ovf`, `*zdiv` out-parameters |
| evaluator | `eval.c:610 int_arith` (1 caller, `:1848`) | type's bytes | `TRAP` (unless `o.wrap`) |
| `#if` | `ppexpr.c:497-600 mul/add/shift` | `intmax_t` | `overflow()` warning, `fail()` |
| presence conditions | `analysis/pcexpr.c:668 fold` | `intmax_t` | nothing (returns `false` for division by zero) |

Two canonicalizers do one job: `cexpr_trunc(Checker*, TypeId, v)` (`cexpr.c:118`, 53 references in 10 files) and `ev_canon(const Ti*, v)` (`eval.c:75`, 39 references in 4 files): truncate to the type's width, then sign- or zero-extend, with `_Bool` as `v != 0`. `eval.c:fits` is a round trip through `ev_canon`.

## Non-goals

- Floating point (`fround`, `fbin`, `fcompare`) and conversions; unary `- ~ !`; relations (AR2 did them).
- Diagnostics and traps: they stay with each caller. The kernel never reports.
- The folder-versus-evaluator split stays (policy per caller).

## Design

New header `src/intarith.h`, static inlines (the evaluator's hot path stays inlined; callgrind must not move by more than 1%).

```c
typedef struct { uint8_t bits; bool sg, boolean; } IaTy;   /* bits 1..64; boolean: sg is false */
enum { IA_OVF = 1, IA_ZDIV = 2, IA_SHIFT_RANGE = 4, IA_SHIFT_NEG = 8, IA_SHIFT_LOSSY = 16 };

uint64_t ia_canon(IaTy t, uint64_t v);                       /* the width, then the extension */
uint64_t ia_binop(int op, IaTy t, uint64_t a, uint64_t b, unsigned *flags);
```

`a` and `b` are canonical for `t` (signed values sign-extended). `ia_binop` always returns a defined value; the flags say what was undefined or lost, and the caller decides what to do:

- `+ - *`: the wrapped result, canonical. `IA_OVF` for signed `t` when the exact result does not fit `t` (computed with `__builtin_*_overflow` on `int64_t`, then a fit test at `bits`; `eval.c` already uses the builtins).
- `/ %`: `b == 0` gives value 0 and `IA_ZDIV`. Signed minimum by -1 gives the wrapped quotient (the minimum) or remainder 0, and `IA_OVF` (for both operators: C11 6.5.5p6 makes the quotient's non-representability undefined for `%` too).
- `& | ^`: canonical.
- `<< >>`: the count is `b` as unsigned (a negative count is huge). Count >= `bits`: `IA_SHIFT_RANGE`; value 0, or all ones for `>>` of a negative signed `a`. Otherwise `<<` gives the canonical `a << b`; `>>` is arithmetic for signed, logical for unsigned. For signed `t` and `<<`: `IA_SHIFT_NEG` when `a < 0`, `IA_SHIFT_LOSSY` when shifting the result back does not give `a` (libcpp's `num_lshift` rule). `IA_OVF` is never set by a shift, so a caller that wants "overflow" from a shift opts in by testing `IA_SHIFT_LOSSY`.

`ia_canon` is `v != 0` for `boolean`; unchanged for `bits >= 64`; otherwise masked, then sign-extended when `sg`. `cexpr_trunc` and `ev_canon` stay as two thin adapters that read `(bits, sg, boolean)` from the Checker's type table and from `Ti` respectively; their bodies are `ia_canon`. `eval.c:fits` becomes `ia_canon(t, x) == x`. (They are adapters over two different type tables, not compatibility shims; the Checker one also passes non-integer and pointer types through unchanged.)

For the two preprocessors, which share libcpp's rule that a negative shift count shifts the other way, `intarith.h` also holds `ia_shift_cpp(op, t, a, b_signed, flags)`: it flips the operator and count for a negative count and calls `ia_binop`. `ppexpr` and `pcexpr` both use it, so the rule has one home.

## Per-site policy (what each caller does with the flags)

| Site | `IA_OVF` | `IA_ZDIV` | `IA_SHIFT_RANGE` | `IA_SHIFT_NEG` | `IA_SHIFT_LOSSY` |
|---|---|---|---|---|---|
| `cexpr` (`int_op`) | `*ovf` | `*zdiv` | ignored | ignored | ignored |
| `eval` | `TRAP` "signed integer overflow" unless `o.wrap` | `TRAP` | `TRAP` | `TRAP` unless `o.wrap` | ignored |
| `ppexpr` | `overflow()` | `fail()` when evaluated | `overflow()` for a nonzero signed left operand of `<<` | ignored | `overflow()` |
| `pcexpr` | ignored | return `false` | ignored | ignored | ignored |

## Differences the kernel would erase (to be decided or pinned before stage 1)

Found by reading the four copies on 2026-10-31; each gets a characterization test before any source changes, and the resolution below is the recommendation.

1. **Signed minimum `% -1`:** `cexpr` flags overflow, `ppexpr` warns, `eval` returns 0 silently, `pcexpr` returns 0 silently. Recommend `IA_OVF` for `%` (the standard says undefined, and x86 `idiv` traps), so `eval` traps where it now returns 0. Behavior change in `eval`; the sweep goldens show whether anything relied on it.
2. **Right shift by a count out of range of a negative signed value:** `ppexpr` and `eval` give all ones; `pcexpr` gives 0. Recommend all ones (libcpp's answer, and `pcexpr` models the same `#if`); behavior change in `pcexpr`.
3. **Negative shift count:** `ppexpr` shifts the other way (libcpp); `pcexpr` gives 0. Recommend the shared `ia_shift_cpp`; behavior change in `pcexpr` (`8 << -1` becomes 4).
4. **Left shift of a negative value:** `eval` traps, `ppexpr` warns through the lossy rule, `cexpr` is silent. Not an erasure: the flags keep each policy.

Items 2 and 3 move `pcexpr` toward `ppexpr`. They need the user's go-ahead each, since they change results; if refused, `pcexpr` keeps a local rule for that case and the kernel is called only where the values agree.

## Tests

- `cereal selftest intarith` (hidden, in `tests/run.sh`): every op over a grid of operands (0, 1, -1, min, max, powers of two, around the width) for `bits` 8, 16, 32, 64, signed and unsigned, checked against `__builtin_*_overflow`, `__int128` arithmetic and a wide reference for the value and flags; `ia_canon` against `(intN_t)` casts. Shown to fail on a deliberately wrong rule.
- Stage 1 characterization (before changing `cexpr`/`eval`): `tests/eval/int_ops.c` through `eval.sh` against gcc, covering the grid at 8/16/32/64 bits with wrap-safe unsigned and defined signed cases; the undefined ones (items 1 and 4) are checked in `cereal selftest` through the callers' flags, since gcc does not define them.
- Stage 2 (before changing `ppexpr`/`pcexpr`): a golden file of `#if` lines over the same grid, expected results from `gcc -E`; how `pcexpr`'s result is observed on it is read at plan time.
- Gate per stage as in `AR2_PLAN.md` (run, san, verify, callgrind); callgrind within 1% of the pre-stage number.

## Stages

1. Kernel, `ia_canon`, selftest; `cexpr` and `eval` call it; the two canonicalizers become adapters. Decide items 1 and 4 here.
2. `ppexpr` and `pcexpr` call it with `bits = 64`; decide items 2 and 3 here.

Each stage is its own commit with its own gate. `int_op`, `int_arith`, the `mul/add/shift` arithmetic bodies and `fold`'s integer cases are deleted in the stage that replaces them.

## Done when

`grep -n "case P_SLASH" src` finds only the kernel, the float code and the non-value switches; the kernel, its selftest and the characterization files are the one home for these facts; `status: done` here and the AR3 item in the cleanup plan records the outcome.

## Review

Stress rounds: 1 (2026-10-31, by hand against the source). Round 1 corrected the reference counts (53 and 39, not about 60 and 38), found that `cexpr`'s first caller is shift-only and ignores its flags, and confirmed the minimum-by-minus-one values in all four copies (`cexpr` minimum or 0 with `ovf`; `eval` `0 - a` or 0; `ppexpr` minimum or 0 with a warning; `pcexpr` `0 - ux` or 0). Remaining unchecked: how `pcexpr`'s results are observable for the stage 2 test, and the count of `case P_SLASH` sites for the Done-when grep (taken at plan time).
