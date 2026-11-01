---
status: active
---

# AR8 implementation plan: unary integer kernel and float binary-op kernel

> **For agentic workers:** execute inline, task by task (`~/.claude/WORKING_STYLE.md`: no subagents for a plan of this shape). Steps use checkbox syntax.

**Goal:** unary `- ~` on integers has one home (`ia_unop` in `src/intarith.h`) and float `+ - * /` has one home (`fl_binop` in the new `src/fparith.h`); the five hand-written unary copies, `eval.c:fits` and both float switches go, and `cexpr` stops double-rounding `double` folds.

**Architecture:** `ia_unop` is `ia_binop(P_MINUS, t, 0, a)` for `-` and `ia_canon(~a)` for `~`; callers keep their overflow policy. `fl_binop(op, FpTy, a, b)` rounds both operands to the named type, does the operation in that type's own precision and returns the result widened to `long double`. `cexpr`'s `fround` and the new `fp_ty` share one type-to-`FpTy` mapping. Stage 1: unary. Stage 2: float.

**Tech stack:** C99, `sh` gate in WSL (`~/cereal-s1`), the Windows tree is the source of truth.

**Spec:** `docs/AR8_DESIGN.md` (approved 2026-10-31, choices a1 b1). This plan amends it in one place: `cexpr` passes the kernel's overflow flag through only when `int_bits <= 64`, as the code does today, because `ia_ty` clamps wider types to 64 bits and would flag a valid `__int128` negate.

## Global constraints

- Edit with the Edit tool. No `python -` on stdin; a script goes in a file and runs with `python3 -P f.py </dev/null`. After any script or `git checkout`, `sed -i 's/\r$//'` the touched files.
- No compatibility wrappers: when a helper goes, every caller is changed in the same commit.
- Build flags are `-std=c99 -pedantic -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wno-unused-parameter`.
- Commit trailer: `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`. Never push.
- The Windows tree is edited; WSL gets it by `sh .sync.sh` in `~/cereal-s1` (rsync; it does not delete files removed on the Windows side, and it keeps mtimes: `touch` a file whose reverted source does not rebuild).
- Results must not change except two: stage 1 makes `eval` trap on negating `INT64_MIN` (`eval.c:fits` was true for every 8-byte type, so only the narrower minima trapped; `cexpr` already warned at 64 bits), and stage 2 changes `cexpr` float folds that double-rounded or converted an integer operand late. Goldens, verify (637/638, 289/289, 3908/3910 identical) and the san findings (0) stay as they are; callgrind within 1% of the pre-stage number (baseline `ar3-t3`: 3 893 519 502).
- Run the check that can fail before relying on a new test: each new test is shown to fail against the old behavior where the plan says so.

## Gate recipe (end of T1 and T2)

Quick loop (6-10 min), from Git Bash:

```bash
MSYS_NO_PATHCONV=1 wsl -e bash -c 'cd ~/cereal-s1 && sh .sync.sh >/dev/null 2>&1; make -j4 2>&1 | grep -E "error|warning"; GOLDEN_JOBS=6 PATH=$HOME/tools/z3:$PATH EVAL_SWEEP_SOLVE="z3 -in -smt2" sh tests/run.sh 2>&1 | tail -3'
```

Expected: no compiler output, last line `N passed, 0 failed`.

Full gate (about 15 min), detached, then a watcher with a deadline (`gate.sh` now exits 1 when a stage prints a failure):

```bash
cd "/c/Users/Winston Li/Documents/GitHub/cereal" && powershell -NoProfile -File "$USERPROFILE/.claude/runs/run.ps1" -Kind gate -Name ar8-tN -Detach -Command "wsl -e bash /home/winstonyli/gate-run.sh" | tail -1
d="$LOCALAPPDATA/claude-runs/cereal/$(date +%F)-ar8-tN"; for i in $(seq 1 55); do grep -q '"end":  null' "$d/run.json" || break; sleep 10; done; grep -E '"end"|"exit"' "$d/run.json"; grep -E "passed|findings|Collected|globals:" "$d/out.log"
```

(Repeat the watcher if it times out first.) Expected in `out.log`: `globals: ... all allowed`, run `N passed, 0 failed`, verify `637 ... differ 1`, `289 ... differ 0`, `3908 ... differ 2`, san `0 failed` and `findings 0`, callgrind `Collected` within 1% of the baseline.

## Review focus

1. Signed minimum negated at every width (8/16/32/64), including the narrow minima whose `0 - a` wraps back. Test: `selftest intarith` (T1), `trap_neg_min.c` and `trap_neg_min64.c` (T1).
2. Unsigned negate wraps with no flag, `~` of `_Bool`-promoted and narrow values stays canonical. Test: `selftest intarith`, `tests/eval/int_ops.c` (T1).
3. `-` of `INT64_MIN` in `#if` warns (`overflow()`), `pcexpr` stays silent. Test: existing `#if` goldens (T1 gate).
4. A `double` add whose exact result sits just above a midpoint of two doubles, and an integer operand above 2^53 mixed with a double. Test: `selftest fparith`, `tests/check/fold_double_round.c` (T2).
5. `float` folds unchanged (the 64-bit intermediate was already innocuous). Test: `selftest fparith` float grid, `d4` in the regression file (T2).
6. `_Float16` and `_Float128` folds keep today's behavior (mapped to `FP_LD`). Test: existing check goldens (T2 gate).

---

### Task 1: `ia_unop` and its five callers

**Files:**
- Modify: `src/intarith.h` (add `ia_unop`)
- Modify: `src/intarith.c` (unary selftest)
- Modify: `src/c/cexpr.c:4150-4160`, `src/c/eval.c:605-610 and 1789-1802`, `src/ppexpr.c:447-462`, `src/analysis/pcexpr.c:1035-1050 and 1286-1295`
- Create: `tests/eval/trap_neg_min.c`, `tests/eval/trap_neg_min64.c`
- Modify: `tests/eval/int_ops.c`, `tests/pp/int_ops.c`, `tests/pc/int_ops.c`, `tests/pc/int_ops.pc`

**Interfaces:**
- Produces: `uint64_t ia_unop(int op, IaTy t, uint64_t a, unsigned *flags)`; `op` is `P_MINUS` or `P_TILDE`; `a` canonical for `t`; result canonical; `*flags` is `IA_OVF` only for `-` of the signed minimum.

- [ ] **Step 1: Add the kernel function.** In `src/intarith.h`, after `ia_binop` and before the `ia_shift_cpp` comment, add:

```c
/* - or ~ of a canonical operand of t; - of the signed minimum wraps and sets IA_OVF */
static inline uint64_t ia_unop(int op, IaTy t, uint64_t a, unsigned *flags)
{
    if (op == P_MINUS)
        return ia_binop(P_MINUS, t, 0, a, flags);
    *flags = 0;
    return ia_canon(t, ~a);
}
```

- [ ] **Step 2: Extend the selftest.** In `src/intarith.c`, in `intarith_selftest`, find

```c
            for (i = 0; i < n; i++)
                for (j = 0; j < n; j++) {
                    Wide x = v[i], y = v[j];
```

and insert before it:

```c
            for (i = 0; i < n; i++) {
                Wide x = v[i];
                unsigned fl;
                uint64_t a, r;
                if (!t_fits(bits, sg, x))
                    continue;
                a = t_wrap(bits, sg, x);
                r = ia_unop(P_MINUS, t, a, &fl);
                if (r != t_wrap(bits, sg, (UWide)0 - (UWide)x) || fl != (sg && x == t_lo(bits, sg) ? IA_OVF : 0u))
                    bad += t_fail("unary -", bits, sg, x, 0, r, fl);
                r = ia_unop(P_TILDE, t, a, &fl);
                if (r != t_wrap(bits, sg, ~(UWide)x) || fl)
                    bad += t_fail("~", bits, sg, x, 0, r, fl);
            }
```

Also change the file's first comment to `ia_binop, ia_unop and ia_canon`.

- [ ] **Step 3: Run it, then show it can fail.** From Git Bash:

```bash
MSYS_NO_PATHCONV=1 wsl -e bash -c 'cd ~/cereal-s1 && sh .sync.sh >/dev/null 2>&1; make -j4 2>&1 | grep -E "error|warning"; ./cereal selftest intarith </dev/null; echo exit $?'
```

Expected: no compiler output, `exit 0`. Then temporarily change `return ia_canon(t, ~a);` in `ia_unop` to `return ia_canon(t, -a);`, `touch src/intarith.h` in WSL after the sync, rebuild and rerun: expected `intarith: ~ ...` lines and `exit 1`. Revert it with the Edit tool and rerun to confirm `exit 0`.

- [ ] **Step 4: Add the characterization tests (they pass before any caller changes).**

`tests/eval/trap_neg_min.c` (new):

```c
// trap: signed integer overflow
int main(void) { int a = -2147483647 - 1; return -a; }
```

`tests/eval/trap_neg_min64.c` (new; traps only after this stage's caller change, so it fails on the old build):

```c
// trap: signed integer overflow
int main(void) { long long a = -9223372036854775807LL - 1; return (int)-a; }
```

`tests/eval/int_ops.c`: in the `UOPS` macro, after the line `h = H(h, (T)(a >> k)); h = H(h, (T)((uint64_t)a << k));                    \` insert a line `        h = H(h, (T)(0 - a)); h = H(h, (T)~a);                                      \`. In the `SOPS` macro, after the line `        h = H(h, (T)(a >> k));                                                     \` (the one followed by the `if (a >= 0 && W < 64` line) insert `        if (x != lo) h = H(h, (T)(-a));                                            \` and `        h = H(h, (T)~a);                                                           \`.

`tests/pp/int_ops.c` and `tests/pc/int_ops.c` (identical bodies; append the same lines to both):

```c
#if ~0 == -1
a15
#endif
#if ~0u == 18446744073709551615u
a16
#endif
#if -1u == 18446744073709551615u
a17
#endif
#if -(-5) == 5 && -(5u) == 18446744073709551611u
a18
#endif
#if ~(-1) == 0 && ~1 == -2
a19
#endif
```

Regenerate the presence-condition golden from WSL and copy it back:

```bash
MSYS_NO_PATHCONV=1 wsl -e bash -c 'cd ~/cereal-s1 && sh .sync.sh >/dev/null 2>&1; cd tests/pc && CEREAL_PCDUMP=1 ../../cereal lint int_ops.c 2>&1 >/dev/null </dev/null | grep "^pc:" > /tmp/int_ops.pc; cat /tmp/int_ops.pc | tail -7; cp /tmp/int_ops.pc "/mnt/c/Users/Winston Li/Documents/GitHub/cereal/tests/pc/int_ops.pc"'
sed -i 's/\r$//' "tests/pc/int_ops.pc"
```

Expected: five new lines with `sat=1 host=1 exact=1`, and `pc: total groups=19 files=1 sys=0 unresolved=0 computed=0 over=0`.

- [ ] **Step 5: Run the quick loop.** Expected: `N passed, 0 failed` with N = 2337 plus the new tests (about 2340). The new tests pass against the old callers.

- [ ] **Step 6: Switch the callers.**

`src/c/cexpr.c`: replace

```c
        if (op == P_MINUS) {
            unsigned bits = int_bits(c, pt);
            if (is_signed(c, pt) && bits <= 64 && bits > 0 &&
                v == cexpr_trunc(c, pt, UINT64_C(1) << (bits - 1)))
                ovf = true;
            v = 0 - v;
        } else if (op == P_TILDE) {
            v = ~v;
        }
```

with

```c
        if (op == P_MINUS || op == P_TILDE) {
            unsigned fl;
            v = ia_unop(op, ia_ty(c, pt), cexpr_trunc(c, pt, v), &fl);
            ovf = (fl & IA_OVF) && int_bits(c, pt) <= 64;
        }
```

`src/c/eval.c`: replace the `P_MINUS` and `P_TILDE` blocks

```c
        if (t->pun == P_MINUS) {
            if (ti.sg && !v->o.wrap && !fits(&ti, (int64_t)(0 - a))) {
                TRAP(v, "signed integer overflow");
                return 0;
            }
            *sh = op_sh(v, SX_NEG, &ti, a, ash, 0, 0);
            return ev_canon(&ti, 0 - a);
        }
        if (t->pun == P_TILDE) {
            *sh = op_sh(v, SX_NOT, &ti, a, ash, 0, 0);
            return ev_canon(&ti, ~a);
        }
```

with

```c
        if (t->pun == P_MINUS || t->pun == P_TILDE) {
            unsigned fl;
            uint64_t r = ia_unop(t->pun, ev_iaty(&ti), a, &fl);
            if ((fl & IA_OVF) && !v->o.wrap) {
                TRAP(v, "signed integer overflow");
                return 0;
            }
            *sh = op_sh(v, t->pun == P_MINUS ? SX_NEG : SX_NOT, &ti, a, ash, 0, 0);
            return r;
        }
```

and delete the function `fits` (`eval.c:605`, its comment included; `grep -n "fits(" src/c/eval.c` must then find nothing).

`src/ppexpr.c` `unary`: replace the `P_MINUS` body lines

```c
        if (!v.uns && sv(v) == INTMAX_MIN)
            overflow(p, op, eval);
        v.v = (uintmax_t)0 - v.v;
```

with

```c
        {
            unsigned fl;
            v.v = ia_unop(P_MINUS, pp_ty(v), v.v, &fl);
            if (fl & IA_OVF)
                overflow(p, op, eval);
        }
```

and `v.v = ~v.v;` in the `P_TILDE` block with `{ unsigned fl; v.v = ia_unop(P_TILDE, pp_ty(v), v.v, &fl); }` written as two statements on separate lines inside braces (house style).

`src/analysis/pcexpr.c` `ev_unary`: declare `unsigned fl;` next to `uint32_t i;` and replace the `n_constu(...)` expression inside `vb_add` with

```c
                   n_constu((intmax_t)ia_unop(n->op, (IaTy){64, !v->u, false},
                                              (uint64_t)v->n, &fl), v->u));
```

In the key reader (around line 1286) replace the two branches `else if (op == P_MINUS) *out = n_constu((intmax_t)(0 - (uintmax_t)a.n), a.u); else if (op == P_TILDE) *out = n_constu(~a.n, a.u);` with one branch

```c
            else if (op == P_MINUS || op == P_TILDE) {
                unsigned fl;
                *out = n_constu((intmax_t)ia_unop(op, (IaTy){64, !a.u, false}, (uint64_t)a.n, &fl), a.u);
            }
```

(keep the `P_PLUS` and `else return false` branches as they are).

- [ ] **Step 7: Grep for leftovers.**

```bash
grep -n "fits(" src/c/eval.c; grep -rn "0 - v\b\|0 - a\b\|(uintmax_t)0 - " src --include=*.c
```

Expected: no `fits`; the remaining `0 - ...` hits are only `cwarn_expr.c:425` (a literal's magnitude), `eval_libc.c:721` (`abs`) and `pcexpr.c:1333` (a key's literal sign): none is a unary operator.

- [ ] **Step 7b: Check the new trap.** `./cereal` on `tests/eval/trap_neg_min64.c` through `tests/eval.sh` (or the quick loop) must now pass; on the pre-step-6 build it failed (the evaluator returned a value). This is the one `eval` behavior change of the stage.

- [ ] **Step 8: Run the gate.** Quick loop first (expected as in step 5), then the full gate `ar8-t1` per the recipe. Expected: as in the recipe. If the evaluator sweeps or the verify run show a changed result, the cause is the 64-bit negate trap: report which function before going on.

- [ ] **Step 9: Commit.**

```bash
git add src tests && git diff --stat && git commit -m "AR8 T1: ia_unop; cexpr, eval, ppexpr and pcexpr use it; fits goes

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 2: `fl_binop`, the regression, and the two float callers

**Files:**
- Create: `src/fparith.h`, `src/fparith.c`, `tests/check/fold_double_round.c`, `tests/check/fold_double_round.expected`
- Modify: `src/main.c` (include, `selftest fparith` dispatch), `tests/run.sh` (selftest line), `src/c/cexpr.c` (`fp_ty`, `fround`, the fold at `:6450`), `src/c/eval.c` (`fbin`)

**Interfaces:**
- Produces: `typedef enum { FP_F32, FP_F64, FP_LD } FpTy;` and `long double fl_binop(int op, FpTy t, long double a, long double b)`; `op` is `P_PLUS`, `P_MINUS`, `P_STAR` or anything else for division (`cexpr`'s default); both operands are first converted to the type; the result is the correctly rounded operation in that type, widened.

- [ ] **Step 1: Write the regression first, and see it fail.** `tests/check/fold_double_round.c`:

```c
/* float folds round once, in the type's own precision (AR8).  Every array has one element when the fold
 * matches the hardware: gcc-13 at run time gives 1+2^-52 for the first sum, 2^53 for the second, 1+2^-23 for the float sum. */
int d1[(1.0 + 0x1.0000002p-53 == 1.0) ? -1 : 1];
int d2[(1.0 + 0x1.0000002p-53 == 0x1.0000000000001p0) ? 1 : -1];
int d3[(1.0 + 9007199254740993 == 9007199254740992.0) ? 1 : -1];
int d4[(1.0f + 0x1.000002p-24f == 0x1.000002p0f) ? 1 : -1];
```

`tests/check/fold_double_round.expected`: an empty file (create it with the Write tool and empty content, or `: > file`). Run it on the unchanged build:

```bash
MSYS_NO_PATHCONV=1 wsl -e bash -c 'cd ~/cereal-s1 && sh .sync.sh >/dev/null 2>&1; cd tests/check && ../../cereal -fsyntax-only -std=c99 -pedantic fold_double_round.c 2>&1 | head'
```

Expected now (red): `error: size of array 'd1' is negative`, and the same for `d2` and `d3`; `d4` is clean (the float case is already right).

- [ ] **Step 2: Write the kernel header.** `src/fparith.h`:

```c
#ifndef CEREAL_FPARITH_H
#define CEREAL_FPARITH_H
#include <float.h>

#include "token.h"

/* + - * / on floating-point constants in the operands' own precision, shared by the constant folder
 * (c/cexpr.c) and the evaluator (c/eval.c).  The folder used to compute in long double and round
 * after, which rounds twice for double. */
#if FLT_EVAL_METHOD != 0
#error "fparith.h needs FLT_EVAL_METHOD == 0: an operation must round to its own type"
#endif

typedef enum { FP_F32, FP_F64, FP_LD } FpTy;

/* a OP b for op = P_PLUS, P_MINUS, P_STAR or (anything else) division: each operand is converted to
 * the type first (the C rule for an operand of another type), the operation is done in the type,
 * and the result is widened */
static inline long double fl_binop(int op, FpTy t, long double a, long double b)
{
    if (t == FP_F32) {
        float x = (float)a, y = (float)b;
        return op == P_PLUS ? x + y : op == P_MINUS ? x - y : op == P_STAR ? x * y : x / y;
    }
    if (t == FP_F64) {
        double x = (double)a, y = (double)b;
        return op == P_PLUS ? x + y : op == P_MINUS ? x - y : op == P_STAR ? x * y : x / y;
    }
    return op == P_PLUS ? a + b : op == P_MINUS ? a - b : op == P_STAR ? a * b : a / b;
}

int fparith_selftest(void);   /* `cereal selftest fparith`: the kernel against hardware float and double */

#endif
```

`src/fparith.c`:

```c
/* fparith.c - `cereal selftest fparith`: fl_binop against the hardware's float and double operations. */
#include <stdio.h>

#include "fparith.h"

int fparith_selftest(void)
{
    static const double vals[] = {0, 1, -1, 3, 0.1, 1e300, 1e-300, 0x1p-24, 0x1.000002p-24, 0x1p-53,
                                  0x1.0000002p-53, 0x1p-52, 9007199254740993.0, 4294967297.0, -0.5};
    static const int ops[] = {P_PLUS, P_MINUS, P_STAR, P_SLASH};
    unsigned i, j, k;
    int bad = 0;
    for (i = 0; i < sizeof vals / sizeof vals[0]; i++)
        for (j = 0; j < sizeof vals / sizeof vals[0]; j++)
            for (k = 0; k < 4; k++) {
                volatile double x = vals[i], y = vals[j], rd;
                volatile float fx = (float)vals[i], fy = (float)vals[j], rf;
                int op = ops[k];
                rd = op == P_PLUS ? x + y : op == P_MINUS ? x - y : op == P_STAR ? x * y : x / y;
                rf = op == P_PLUS ? fx + fy : op == P_MINUS ? fx - fy : op == P_STAR ? fx * fy : fx / fy;
                if (fl_binop(op, FP_F64, vals[i], vals[j]) != (long double)rd &&
                    !(rd != rd && fl_binop(op, FP_F64, vals[i], vals[j]) != fl_binop(op, FP_F64, vals[i], vals[j]))) {
                    fprintf(stderr, "fparith: double %g %d %g\n", vals[i], op, vals[j]);
                    bad++;
                }
                if (fl_binop(op, FP_F32, vals[i], vals[j]) != (long double)rf &&
                    !(rf != rf && fl_binop(op, FP_F32, vals[i], vals[j]) != fl_binop(op, FP_F32, vals[i], vals[j]))) {
                    fprintf(stderr, "fparith: float %g %d %g\n", vals[i], op, vals[j]);
                    bad++;
                }
            }
    /* the midpoint case: 1 + (2^-53 + 2^-80) is above the midpoint, so one rounding gives 1 + 2^-52 */
    if (fl_binop(P_PLUS, FP_F64, 1.0L, 0x1.0000002p-53L) != 0x1.0000000000001p0L) {
        fprintf(stderr, "fparith: midpoint case\n");
        bad++;
    }
    /* an integer operand is rounded to double before the add */
    if (fl_binop(P_PLUS, FP_F64, 1.0L, 9007199254740993.0L) != 9007199254740992.0L) {
        fprintf(stderr, "fparith: operand conversion\n");
        bad++;
    }
    if (bad)
        fprintf(stderr, "fparith: %d failures\n", bad);
    return bad != 0;
}
```

(NaN results compare unequal to themselves; the guard accepts a NaN on both sides. If the project warning flags object to `-Wfloat-equal`-style patterns, the build output tells; there is no such flag in the list.)

- [ ] **Step 3: Wire the selftest.** In `src/main.c` add `#include "fparith.h"` next to `#include "intarith.h"` and, after the `intarith` dispatch (`main.c:1601`), add

```c
    if (argc == 3 && !strcmp(argv[1], "selftest") && !strcmp(argv[2], "fparith"))
        return fparith_selftest();
```

In `tests/run.sh` after the `selftest intarith` line (`:1059`) add

```sh
if "$CEREAL" selftest fparith </dev/null; then ok; else bad "selftest fparith"; fi
```

- [ ] **Step 4: Run the selftest, then show it can fail.**

```bash
MSYS_NO_PATHCONV=1 wsl -e bash -c 'cd ~/cereal-s1 && sh .sync.sh >/dev/null 2>&1; make -j4 2>&1 | grep -E "error|warning"; ./cereal selftest fparith </dev/null; echo exit $?'
```

Expected: no compiler output, `exit 0`. Then temporarily make the `FP_F64` branch compute `return op == P_PLUS ? a + b : ...` in `long double` (the old rule), `touch src/fparith.h`, rebuild, rerun: expected `fparith: double ...`, `fparith: midpoint case` and `exit 1`. Revert and rerun to confirm `exit 0`.

- [ ] **Step 5: Switch `cexpr`.** In `src/c/cexpr.c` add `#include "fparith.h"` after `#include "intarith.h"`. Replace `fround` (`:173`) with a mapping and a `fround` built on it:

```c
static FpTy fp_ty(Checker *c, TypeId t)
{
    switch (tkind(c, t)) {
    case TY_FLOAT: case TY_FLOAT32:
        return FP_F32;
    case TY_DOUBLE: case TY_FLOAT64: case TY_FLOAT32X:
        return FP_F64;
    case TY_LDOUBLE:
        return c->tgt->long_double == LD_IEEE64 ? FP_F64 : FP_LD;
    default:
        return FP_LD;
    }
}

static long double fround(Checker *c, TypeId t, long double v)
{
    switch (fp_ty(c, t)) {
    case FP_F32:
        return (float)v;
    case FP_F64:
        return (double)v;
    default:
        return v;
    }
}
```

In the binary fold (`:6450`) replace

```c
        long double r = 0;
        switch (op) {
        case P_PLUS: r = fa + fb; break;
        case P_MINUS: r = fa - fb; break;
        case P_STAR: r = fa * fb; break;
        default: r = fa / fb; break;
        }
        c->ck[i] = K_FLOAT;
        c->cv[i] = fpush(c, fround(c, rt, r));
```

with

```c
        c->ck[i] = K_FLOAT;
        c->cv[i] = fpush(c, fl_binop(op, fp_ty(c, rt), fa, fb));
```

- [ ] **Step 6: Switch `eval`.** In `src/c/eval.c` add `#include "fparith.h"` after `#include "intarith.h"` and replace the whole body of `fbin` (`:518`, both switches) with

```c
static uint64_t fbin(Ev *v, int p, const Ti *ti, uint64_t a, uint64_t b)
{
    if (p != P_PLUS && p != P_MINUS && p != P_STAR && p != P_SLASH) {
        UNSUP(v, "float operator %d", p);
        return 0;
    }
    return ev_fput(ti, (double)fl_binop(p, ti->bytes == 4 ? FP_F32 : FP_F64, ev_fget(ti, a), ev_fget(ti, b)));
}
```

- [ ] **Step 7: Run the regression and the quick loop.** The regression command of step 1 must now print nothing (if a pedantic warning appears for a valid fold, compare it with `gcc-13 -fsyntax-only -std=c99 -pedantic` on the same file; record it in `.expected` only when gcc prints the same). Quick loop: `N passed, 0 failed`, N = the T1 count plus 2 (the new check golden and the selftest line).

- [ ] **Step 8: Run the gate.** Full gate `ar8-t2` per the recipe. The evaluator sweeps and the verify run are the checks that `eval`'s float results did not move.

- [ ] **Step 9: Commit.**

```bash
git add src tests && git diff --stat && git commit -m "AR8 T2: fl_binop; cexpr folds float ops in the type's precision; fbin is a call

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Close

**Files:**
- Modify: `docs/AR8_DESIGN.md`, `docs/AR8_PLAN.md`, `docs/CLEANUP_PLAN.md`, `docs/EVAL_DESIGN.md:19`

- [ ] **Step 1: Docs.** In `docs/EVAL_DESIGN.md:19` change `integer ops in `src/intarith.h`` to `integer ops in `src/intarith.h`, float ops in `src/fparith.h``. Grep for stale wording: `grep -rn "fits\b" docs/*.md | grep -v "AR3_\|AR8_"` and fix any claim that `eval.c:fits` exists. In `AR8_DESIGN.md` set `status: done`, replace "Not yet..." review text's "Round 2 pending" leftovers if any, and add an `## Outcome` section (commits, line counts, the two behavior changes: `cexpr` `double` folds round once and mixed integer/double folds convert the integer first). Set this plan `status: done`. In `CLEANUP_PLAN.md` append `**Done 2026-10-31**` to the AR8 item with the commits, the actual size against the spec and the gate names. If the open question about decimal float literals was not answered during the work, add it to `PARKED.md` with its trigger (a literal that differs from gcc's) and link it from the item.
- [ ] **Step 2: Final check.** Quick loop, `git status --short` (nothing uncommitted), then commit:

```bash
git add docs && git commit -m "AR8: close the plan; docs name fparith.h

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

## Self-review

- Spec coverage: unary kernel and its five callers (T1), `fits` deleted (T1 step 6), float kernel and both callers (T2), `FLT_EVAL_METHOD` guard (T2 step 2), operand conversion (T2 `d3` and selftest), `_Float16`/`_Float128` unchanged (`fp_ty` default), tests and gate per stage, decimal-literal open question closed or parked (T3).
- Placeholder scan: none; every edit has the old and new text.
- Type consistency: `IaTy`, `IA_OVF`, `ia_unop(op, t, a, &flags)` match between T1 steps; `FpTy`, `fl_binop(op, t, a, b)` and `fp_ty(c, rt)` match between T2 steps.
- Risk: `tests/eval/int_ops.c` shares macros across widths; if the `SOPS` anchor line is not unique, include the following `if (a >= 0` line in the Edit's old text.
