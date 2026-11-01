---
status: draft
---

# AR8: a unary integer kernel and a float binary-op kernel

Item AR8 of `docs/CLEANUP_PLAN.md`; follows AR3 (`docs/AR3_DESIGN.md`, done). Scope chosen 2026-10-31 (the user's "as rec, park b3"): one spec and plan for both kernels, staged unary then float; the float kernel is a new header, binary `+ - * /` only.

## Problem

**Unary `-` and `~` on integers** are written out five times, each with its own overflow policy:

| Site | Where | Reports through |
|---|---|---|
| constant folding | `cexpr.c:4138-4160` (`v = 0 - v`, with a hand-written test for the signed minimum; `~v`) | `ovf` to `unary_value` |
| evaluator | `eval.c:1790-1801` (`fits(&ti, 0 - a)` then `ev_canon(0 - a)`; `ev_canon(~a)`; `fits` has this one caller) | `TRAP` unless `o.wrap` |
| `#if` | `ppexpr.c:449-466` | `overflow()` warning for the minimum |
| presence conditions | `analysis/pcexpr.c:1035 ev_unary` | nothing |
| the same fold, from a key | `analysis/pcexpr.c:1286` | nothing |

`!` is `a == 0` everywhere and has nothing to share; it stays.

**Float `+ - * /`** is written twice with different precision:

- `eval.c:518 fbin` computes in the type's own precision (float arithmetic for `float`, double for `double`).
- `cexpr.c:6456` computes in `long double`, then `fround`s to the type. For `double` this rounds twice and is wrong when the exact result sits just above a midpoint that the 64-bit mantissa rounds onto: `1.0 + 0x1.0000002p-53` folds to 1.0 in `cereal check`, where gcc's runtime add (and correct rounding) gives `1 + 2^-52`. Found and confirmed 2026-10-31 (`gcc-13` runtime `a+b==1.0` is 0; `cereal check` rejects `int a[(1.0 + 0x1.0000002p-53 == 1.0) ? -1 : 1]` as a negative array size). `float` is not affected (64 >= 2*24+2: the double rounding is innocuous).

## Non-goals

- Unary `!`, unary `+` (the operand, promoted), float negate (exact), conversions, compares (AR2's `rel_holds`), complex (`cplx_arith`) and decimal floats: they keep their paths. Folding negate, conversion and compare into the float kernel is parked (`PARKED.md`, "Float kernel beyond `+ - * /`").
- Diagnostics and traps stay with each caller; the kernels never report.
- Decimal floating *literals*: `lex`'s `long double` parse followed by `fround` (`cexpr.c:1298`) may double-round the same way. Not tested (the check on 2026-10-31 was inconclusive: `cereal check` did not fold a literal compare). Listed under Open questions, not in scope.

## Design

### Stage 1: `ia_unop` in `src/intarith.h`

```c
uint64_t ia_unop(int op, IaTy t, uint64_t a, unsigned *flags);   /* op is P_MINUS or P_TILDE; a canonical for t */
```

`P_MINUS`: the canonical wrapped negation; `IA_OVF` when `t` is signed and `a` is the minimum of `t`. `P_TILDE`: the canonical complement, no flags. `a` is already promoted (callers promote first, so `t` is `int` or wider).

| Site | `IA_OVF` |
|---|---|
| `cexpr` | `ovf` to `unary_value` (replaces the hand-written minimum test) |
| `eval` | `TRAP` "signed integer overflow" unless `o.wrap`; `fits` (`eval.c:605`) loses its only caller and is deleted (AR3 kept it for exactly this use) |
| `ppexpr` | `overflow()` |
| `pcexpr` (both) | ignored |

`cexpr` keeps its `int_bits <= 64` guard (wider types do not reach the kernel). `pcexpr.c:1333` (a key's literal sign) is a literal read, not an operator, and stays. `ppexpr` and `pcexpr` pass `bits = 64`, signed or unsigned from the value (`Val.uns`, `Num.u`).

### Stage 2: `fl_binop` in `src/fparith.h`

```c
typedef enum { FP_F32, FP_F64, FP_LD } FpTy;
long double fl_binop(int op, FpTy t, long double a, long double b);   /* op: P_PLUS P_MINUS P_STAR P_SLASH */
```

The operation is done in the named precision: `F32` as `float x float`, `F64` as `double x double`, `LD` as `long double`; the result is widened. The kernel first converts `a` and `b` to the type (`(float)a`, `(double)a`), which is the C rule for an operand of another type and a single rounding of an exact value: this also fixes `1.0 + 9007199254740993` (the integer operand is rounded to `double` before the add; `cexpr` adds it exactly in `long double`; confirmed 2026-10-31: gcc's runtime `1.0 + (double)9007199254740993 == 9007199254740992.0` is 1, `cereal check` rejects `int a[(1.0 + 9007199254740993 == 9007199254740992.0) ? 1 : -1]` as a negative array size). Division by zero gives the IEEE result; `cexpr` keeps its own guard (it does not fold `x / 0`) and `eval` keeps its behavior.

- `cexpr.c:6456` maps the type to an `FpTy` (`TY_FLOAT`, `TY_FLOAT32` to `F32`; `TY_DOUBLE`, `TY_FLOAT64`, `TY_FLOAT32X`, and `TY_LDOUBLE` on an IEEE-64 `long double` target to `F64`; other kinds, including `_Float16` and `_Float128`, to `LD`, as `fround` treats them today), calls `fl_binop`, and drops the `fround` for these four operators. `fround` stays for literals, conversions and complex.
- `eval.c:518 fbin` keeps `ev_fget`/`ev_fput` and calls `fl_binop` (F32 or F64; the evaluator has no `long double`), so its two switch bodies go.
- The header carries `#if FLT_EVAL_METHOD != 0` / `#error`: a float operation done in extended precision would reintroduce the bug (true on x86-64 with SSE, not on 32-bit x87).

Behavior change: `cexpr` folds of `double` operations that double-rounded now round once. `eval` is unchanged.

## Tests

- `selftest intarith` gains `ia_unop` over the same grid (`bits` 8-64, signed and unsigned, against `__int128` negation and complement; the signed minimum flags `IA_OVF`).
- Stage 1 characterization: `tests/eval/int_ops.c` gains `-` and `~` over the grid; `tests/pp/int_ops.c` and `tests/pc/int_ops.c` gain the unary lines (expected results from `gcc -E` where defined).
- `cereal selftest fparith`: `fl_binop` against hardware `float` and `double` operations over a grid including the midpoint case above; shown to fail with the old `long double` rule.
- Stage 2 regression: `tests/check/fold_double_round.c`, the array-size declarations above (the `1.0 + 0x1.0000002p-53` pair and the `9007199254740993` case), plus the gcc runtime value in the comment.
- Gate per stage as in `AR3_PLAN.md` (run, san, verify, callgrind within 1% of the pre-stage number).

## Stages

1. `ia_unop`, its selftest and tests; the five integer sites call it.
2. `fparith.h`, its selftest and the regression; `cexpr` and `eval` call it.

Each stage is its own commit with its own gate. The hand-written negate/complement bodies and `fbin`'s two switches are deleted in the stage that replaces them.

## Open questions

- Are decimal float literals (`cexpr.c:1298`) affected by the same double rounding? Test with a decimal literal one `long double` ulp above a double midpoint, folded and at runtime, before the plan; if so, a third stage (parse at the type's precision) or a parked item.

## Done when

`grep -n "0 - v\|0 - a" src` finds no unary negate outside the kernel; `fbin` is a call and `fits` is gone; `selftest intarith` and `selftest fparith` are the homes for these facts; `status: done` here and the AR8 item in the cleanup plan records the outcome.

## Review

Round 1 (2026-10-31, by grep against the source): `fits` has one caller, so AR8 deletes it (AR3's note that it stays held only until now); `pcexpr.c:1333` is a literal read, not in scope; the kernel must round the operands to the type, which also changes mixed integer/double folds; `_Float16`/`_Float128` keep today's behavior. Round 2: the mixed integer/double fold was confirmed against gcc's runtime; no other claim changed. A third pass found nothing further.
