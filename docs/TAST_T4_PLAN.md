---
status: done
---

# TAST stage T4: plan

Stage T4 of `docs/TAST_DESIGN.md` (section 5, "floating point, `long double`,
`_Complex`, `__int128`, decimal and `_FloatN`"), built on T3
(`docs/TAST_T3_PLAN.md`, results in its section 9). Facts are from the tree
and the coverage runs of 2026-10-18; anything not yet read or measured is
marked **verify**. Built; results in section 10.

## 1. Why, and what the numbers say

After T3 the unsupported units are (first reason per unit):

| Reason | lua54 (206) | libuv (20) |
|---|---|---|
| floating type | 190 | 12 |
| everything else (variadic definition, `va_arg`, address-of-label, compound lvalue with side effects, `asm`, ...) | 16 | 8 |

Floating point is 92% of lua54's remaining units and 60% of libuv's, so it is
the only bucket worth a stage. `_Complex`, `__int128`, `_FloatN` and decimal
types do not occur in the first reason of any unit of either corpus
(**verify**: count them over the second reason too, by letting the pass
carry on past the first one in a throwaway run); they are split off into T4b
and stay `unsupported` until a corpus needs them.

## 2. Scope: T4a is `float`, `double`, `long double`

In: the three real floating types as scalars (locals, parameters, return
values, struct members, arrays, static data), floating constants, arithmetic
(`+ - * /` and unary `-`), comparisons, truth tests, conversions between any
two of {integers, `_Bool`, `float`, `double`, `long double`}, calls with
floating arguments and results (including the default promotion
`float -> double` in variadic and unprototyped calls, which T3a left open),
`++`/`--` and compound assignment on floating lvalues, `?:` with floating
arms.

Out (T4b, unsupported with their existing reasons): `_Complex`, `__int128`,
`_Float16/32/64/128`, `_Float32x/64x`, decimal, vector types, `%` and the
bitwise operators on floating operands (the checker already rejects them).

## 3. The constant representation (the decision design 3.6 asked for)

A `const` node holds an `int64_t`. A floating constant does not fit it
(`long double` is 80 bits in a 16-byte object on x86-64), and `Checker.cv`
indexes `Checker.fv`, a vector of host `long double`.

Design: a new value op **`fconst`** holding the constant's **bytes in the
target format** (4, 8 or 16), stored in `El.bytes` exactly like a `str`'s
code units (offset in `val`, length in `width`) and printed as hex:

    #3 fconst 3ff0000000000000 double

One storage mechanism for string data and float data; no second vector, no
change to `TNode`. A consumer reads the bytes; it never sees a host float.

Why bytes and not a host `double` field or a decimal string: the dump and
the data oracle compare bit patterns (NaN payloads, signed zero,
denormals); a host value hides the target format question; a decimal string
pushes rounding onto the consumer.

Conversion at the pass boundary, one function `fbits(Checker*, long double,
TypeId) -> bytes`:

- `float`: the nearest `float` of the host value; `double`: nearest `double`;
  `long double` by `Target.long_double`: `LD_X87` the host's 80-bit value
  when the host long double is x87 (**verify** the host here is x86-64
  Linux/WSL, `LDBL_MANT_DIG == 64`; a guard decides at run time), `LD_IEEE64`
  the `double` bytes; `LD_IEEE128` and `LD_IBM128` make the unit
  `unsupported: long double format`.
- **Double rounding, measured and fixed (2026-10-18).** The checker parsed
  every literal with `strtold` (64-bit mantissa) and then rounded to the
  type, so `9007199254740993.00000001` became `...992` where gcc gives
  `...994` (a checker bug visible to `_Static_assert`, not only to the
  pass). `lit.c` now parses with `strtof`/`strtod`/`strtold` by the literal's
  type, one rounding; `tests/check/float_literal_single_round.c` pins it
  (ties and just-above-ties, float, double, hex). `fv` therefore holds a
  value already exact in the node's type.
- Verifier for `fconst`: no kids, floating type, `width` equals the type's
  size, bytes inside `El.bytes`.

`ex` folds a `K_FLOAT` node, and an integer-valued `K_ICE`/`K_FOLD` node
whose type is floating, into `fconst`, as it folds integers into `const`
today (`tast.c:732`; `K_FLOAT` is not folded there now). **Verify** that a
`K_FLOAT` value in `fv` is already rounded to its node's type (for example
`(float)0.1` holds the `float` value), so `fbits` only changes the format.

`conv()` retypes a `const` in place with `cexpr_trunc`, an integer
truncation; that is wrong for a floating target. The rule, minimal and
without a second folder: a `const` converted to a floating type becomes an
`fconst` of the exact integer (a 64-bit integer is exact in the host
`long double`, so `fbits` rounds once); an `fconst` converted to anything
else stays a `cast` node. The checker has already folded every constant
expression it can at the outermost node, so such casts are rare, and the
pass never folds a float-to-integer conversion (out of range it is
unspecified in gcc, and a guessed value would be wrong).

## 4. Operators and conversions

- `ok_ty` accepts `TY_FLOAT`, `TY_DOUBLE`, `TY_LDOUBLE`; `why_type`'s default
  ("floating type") then only fires for the T4b kinds, and gains one line
  each for `_FloatN`/decimal so the histogram says which (`complex type`
  and `__int128 type` exist already).
- Usual arithmetic conversions come from `common_type`, which already
  implements gcc's `c_common_type` for floats (`cexpr_int.h:357`); the
  existing `agree()` check compares the checker's result type with the rule
  and stays the safety net. FLT_EVAL_METHOD is 0 on x86-64: each operation
  is evaluated in its own type, so no excess-precision node is needed
  (**verify** `-m32`/x87 targets are not in scope: the pass runs with the
  given `--target`, and a target whose evaluation method is not 0 makes the
  unit unsupported).
- Integer promotions do not apply to floating operands; `float` stays
  `float` except as a variadic or unprototyped extra argument, where C
  promotes it to `double`. **T3a gap, found in review:** `call_args` takes
  the extra arguments' type from `promoted()` (`cexpr_int.h:225`), which
  returns a non-integer type unchanged, so a `float` extra argument would
  stay `float`. Fix in `call_args` (convert to `double`) and in the
  verifier's extra-argument check ("`float` extra argument not promoted").
  Both fixes landed ahead of T4; no floating value reaches them until
  `ok_ty` admits `float`, so their golden case (`printf("%f", f)`) belongs
  to `tests/tast/float.c`.
- Truth value (`!x`, `&&`, `||`, `?:` condition, `if`/loops, `(_Bool)x`) of a
  floating operand means `x != 0` (NaN is true). Stated once in the dump
  header notes and in the verifier comment; no node change, because the
  operand type is already on the kid. Verifier rules for these nodes do not
  constrain the operand type today, so nothing to loosen (**verify** `TO_UNOP`
  `!` and `TO_CAST` to `_Bool`).
- `++`/`--` and compound assignment build their `1` and their promoted
  operand through `cnst(...)`, an integer constant of the operand type
  (`incdec`, `tast.c:480-485`). For a floating operand the `1` must be an
  `fconst 1.0` of the operand type. One helper `one_of(type)` used by both,
  replacing the two `cnst(.., 1, ..)` calls.
- The verifier's `BINOP`/`CMP`/`UNOP` type rules (**verify** in `tast.c`: do
  they require integer operand types, or pointer-vs-integer shape only?) are
  widened to allow floating operands with equal operand types; `neg` on a
  float is the only unary arithmetic op, `~` stays integer-only.
- Comparisons of floats keep the same `cmp` node; `-0.0 == 0.0` and NaN
  semantics are the target's, not the node's.
- Switch and `case` on a floating value: rejected by the checker.
- `ptradd` index and array subscripts stay integer.

## 5. Static data

`double d = 1;`, `float f[3] = {1, 2.5f}`, `long double x = 1.0L`, struct
members, arrays, pointers-to-double are all `elem` values; the value tree is
an `fconst` (or a `cast`/`fconst` the checker already folded). `sconst`
(the verifier's static-constness rule) accepts `fconst` and a `cast` of
constants as before. The data oracle (`tests/tast_data.py`) evaluates
`fconst` as its bytes, `cast` int to float with `struct.pack`, `cast` float
to integer by truncation toward zero (the static-data case `int x = 1.5;`), and
`long double` from the 16-byte x87 image (padding bytes zero: **verify**
that gcc emits zero for the six padding bytes of a static `long double`; if
it does not, the oracle masks them and the dump must say so).

## 6. Order, checks, gates

1. **Spike (read-only, one hour):** run the pass with `ok_ty` extended and
   everything else unchanged over lua54 and libuv; histogram the *next*
   reason of the 190 + 12 units. This says what T4a really unlocks and
   catches a surprise operator or construct.
2. `ok_ty`, `fbits`, `fconst` (node, printing, verifier, `El.bytes` reuse),
   `conv()` fix, `one_of`, promotion rule. Each lands with golden cases in
   a new `tests/tast/float.c`.
3. Data: float elements; extend `tests/tast_data.py`; `tests/tast/fdata.c`
   with ties, denormals, `-0.0`, infinities, NaN (`__builtin_nan`
   is a call: only as a constant if the checker folds it, **verify**),
   `long double`, floats in structs and arrays.
4. Gates: suite; `--dump-symbols --verify-tast` clean; `tests/tast_gcc.py`
   extended with `float|double` in `TYPEWORDS` (casts to and from float are
   in gcc's `-fdump-tree-original` as `(double)` etc.); data oracle on
   lua54 (its `lua_Number` statics) and libuv; coverage via `run.ps1`;
   callgrind unchanged (nothing in the checker changes unless the double
   rounding check fails).

Expected result (a bound, not a promise): lua54's 190 floating units move on
to their next reasons; the spike gives the real number.

### Spike result (2026-10-18, throwaway edit in `~/cereal-s1`, reverted by the next rsync)

`ok_ty` admitting `float`/`double`/`long double`, nothing else changed:

| | lua54 | libuv |
|---|---|---|
| unsupported before | 206 | 20 |
| unsupported after | 38 | 12 |
| `expression NUMBER` (a `K_FLOAT` constant, no `fconst` yet) | 18 | 4 |
| `assignment operator` (compound assignment on a float) | 4 | 0 |
| unrelated, unchanged | 16 | 8 |

So T4a unlocks 168 of lua54's 190 floating units and 8 of libuv's 12
outright from `ok_ty`; `fconst` and float compound assignment take the
remaining 22 and 4. Verifier findings stayed 0, but only because the
verifier does not constrain floating operands yet (section 4): the 0 is not
evidence of correct trees. The ceiling is lua54 16 and libuv 8 unsupported,
all non-floating.

## 7. Risks and what is deliberately out

- Host `long double` assumption (above). A cross-target run on a non-x87
  host cannot be exact; it becomes `unsupported`, never wrong.
- Double rounding of literals (above); the oracle decides whether the
  checker changes.
- A float-to-integer cast of an out-of-range constant is left as a `cast`.
- Floating `switch`, `%`, bit operations: rejected upstream.
- `_Complex` and the rest of T4b: parked until a corpus needs them.
- Unspecified evaluation order and contraction (`-ffp-contract`): the node
  tree says what the source says; contraction is a back-end choice.

## 8. Parked by this plan (to add to `PARKED.md` when T4 starts)

- T4b: `_Complex`, `__int128`, `_FloatN`, decimal (trigger: a corpus unit
  whose first or second reason is one of them).
- Non-x87 `long double` formats and targets with FLT_EVAL_METHOD != 0
  (trigger: a back end for such a target).

## 9. Review log

- **Round 1 (claims against the tree).** Caught: `call_args` would leave a
  `float` variadic argument as `float` (`promoted()` ignores non-integers):
  a T3a gap, now in section 4. `why_type` has no `_FloatN` line. The
  `common_type` line number was the table, not the declaration.
- **Round 2 (design holes).** Caught: `conv()` folds constants with an
  integer truncation, wrong for floating targets; the first draft had the
  pass fold `fconst` conversions, a second folder, replaced by "convert
  `const` to `fconst`, leave every other conversion a `cast`". `fconst`
  verifier rules were missing. `LD_IEEE64` is a trivial case, not
  unsupported. The data oracle needs float-to-integer truncation for
  `int x = 1.5;`. A stray remark about `lua_Number` unions in the spike was
  removed (not a float question).
- **Round 3 (verifier and consumers).** Caught: the verifier's operand-type
  rules for `BINOP`/`CMP`/`UNOP` were not addressed; added as a **verify**
  item in section 4. A split sentence in section 3 repaired.
- **Round 4.** Nothing material: the sections agree on one representation
  (`fconst` bytes), one conversion rule, one rounding path (`fv` once),
  and the open items are all marked **verify** with how to check them.

## 10. Progress

### T4a, built (2026-10-18, commit `b4953bb`; literal rounding `3ba33ba`)

- `fconst` op: target-format bytes in `El.bytes` (`fset`), printed most
  significant byte first; verifier checks kids, type, size, range. `ok_ty`
  admits `float`/`double`/`long double`; `ex` folds `K_FLOAT`; an
  integer-valued constant of floating type is `unsupported`.
- `conv()`: an integer `const` converted to a floating type becomes `fconst`
  in place; every other conversion of a constant is a `cast`.
- `++`/`--` use an `fconst 1`; compound assignment admits floating lvalues.
- Verifier: floating `binop` is `+ - * /` only, floating `unop` is `-` only
  (the existing same-type rules already covered operands). Variadic `float`
  extra arguments promote to `double` (`5956e2c`).
- `why_type`: `_FloatN type`, `decimal floating type`, and a last
  `unsupported type` default.
- Checker bug found and fixed on the way: `lit.c` rounded floating literals
  twice (via `strtold`); now once, with
  `tests/check/float_literal_single_round.c`.
- Tests: `tests/tast/float.c` (functions), `tests/tast/fdata.c` (static data,
  32 objects against gcc bytes, ties, denormals, `-0.0`, infinities, x87
  with zero padding), `tests/tast/unsup.c` moved to `__int128`/`_Complex`.
  `tests/tast_gcc.py` casts include `float`/`double`: all float functions
  agree with gcc's tree.
- Coverage (`2026-10-18-tast-t4a-coverage`): lua54 206 -> 16 unsupported
  (variadic definition 10, compound lvalue 2, `va_arg` 2, address-of-label 2),
  libuv 20 -> 8 (array type 2, one each definition, `asm`, lvalue form,
  `va_arg`, `setproctitle`, `__atomic_fetch_add`); none floating. 0 verifier
  findings; data oracle lua54 107 objects, libuv 32, fdata 32, 0 failures.
- Gate (`2026-10-18-t4a-gate`): suite and san 2256 passed, san findings 0,
  verify differ unchanged (2/1/0), callgrind 3,894,829,282 (T3: 3,895,360,854).
- Open items from the plan, closed: `K_FLOAT` values are already rounded to
  their type (the spike and `fdata.c` ties agree with gcc); verifier
  operator rules widened as above; `-ffp-contract` and FLT_EVAL_METHOD
  untouched (x86-64 only, others `unsupported: long double format` or equal).
  Leftovers are in `PARKED.md` ("TAST T4 leftovers").
