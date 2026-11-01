---
status: done
---

# AR3 implementation plan: integer-op kernel and canonicalizer

> **For agentic workers:** execute inline, task by task (`~/.claude/WORKING_STYLE.md`: no subagents for a plan of this shape). Steps use checkbox syntax.

**Goal:** `+ - * / % & | ^ << >>` and the width canonicalization each have one home (`src/intarith.h`); `int_op`, `int_arith`, the arithmetic bodies of `ppexpr.c` and `fold`'s integer cases go.

**Architecture:** header-only kernel `ia_binop`/`ia_canon` returning a defined value plus flags; each caller keeps its policy. `cexpr_trunc` and `ev_canon` stay as thin adapters over `ia_canon`. Stage 1: kernel, `cexpr`, `eval`. Stage 2: `ppexpr`, `pcexpr`.

**Tech stack:** C99, `sh` gate in WSL (`~/cereal-s1`), Windows tree is the source of truth.

**Spec:** `docs/AR3_DESIGN.md` (approved 2026-10-31; decisions p1 and p2 recorded there). This plan amends it in one place: for a signed `<<` with a count of at least the width, `IA_SHIFT_LOSSY` is also set when the operand is nonzero, so `ppexpr`'s "overflow on a left shift out of range" is just `IA_SHIFT_LOSSY` (the spec's per-site table says `ppexpr` tests `IA_SHIFT_RANGE` for it; it tests `IA_SHIFT_LOSSY` alone).

## Global constraints

- Edit with the Edit tool. No `python -` on stdin; a script goes in a file and runs with `python3 -P f.py </dev/null`. Python writes with `newline=''`; after any script or `git checkout`, `sed -i 's/\r$//'` the touched files.
- No compatibility wrappers: when a helper goes, every caller is changed in the same commit. The two adapters (`cexpr_trunc`, `ev_canon`) are not wrappers: they read `(bits, sg, boolean)` from two different type tables.
- Build flags are `-std=c99 -pedantic -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes`; `__int128` needs `__extension__`.
- Commit trailer: `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`. Never push.
- The Windows tree is edited; WSL gets it by `sh .sync.sh` in `~/cereal-s1` (rsync; it does not delete files removed on the Windows side).
- Results must not change except the two decided behavior changes: `INT_MIN % -1` traps in `eval` (stage 1), and `pcexpr` follows `ppexpr` for out-of-range and negative shift counts (stage 2). Goldens, verify (3908/3910 identical, 637/638, 289/289) and san findings 0 stay as they are.
- The kernel text below was compiled and run in a scratch directory on 2026-10-31 (`-std=c99 -pedantic` and the project warning flags clean; the selftest passes; a deliberately wrong `%` rule fails it with 80 failures). `tests/eval/int_ops.c` was run under `-fsanitize=undefined` (no report) and through the current `cereal run --eval-wrap` (output identical to gcc).

## Gate recipe (used at the end of T2 and T3)

Quick loop (6-10 min), from Git Bash:

```bash
MSYS_NO_PATHCONV=1 wsl -e bash -c 'cd ~/cereal-s1 && sh .sync.sh >/dev/null 2>&1; make -j4 2>&1 | grep -E "error|warning"; GOLDEN_JOBS=6 PATH=$HOME/tools/z3:$PATH EVAL_SWEEP_SOLVE="z3 -in -smt2" sh tests/run.sh 2>&1 | tail -3'
```

Expected: no compiler output, last line `N passed, 0 failed`.

Full gate (about 15 min), detached, then a watcher with a deadline:

```bash
cd "/c/Users/Winston Li/Documents/GitHub/cereal" && powershell -NoProfile -File "$USERPROFILE/.claude/runs/run.ps1" -Kind gate -Name ar3-tN -Detach -Command "wsl -e bash /home/winstonyli/gate-run.sh" | tail -1
d="$LOCALAPPDATA/claude-runs/cereal/$(date +%F)-ar3-tN"; for i in $(seq 1 55); do grep -q '"end":  null' "$d/run.json" || break; sleep 10; done; grep -E '"end"|"exit"' "$d/run.json"; grep -E "passed|findings|Collected|globals:" "$d/out.log"
```

(Repeat the watcher if it times out first; Bash calls over 600 s move to the background.) Expected in `out.log`: `globals: ... all allowed`, run `N passed, 0 failed`, verify `637 ... differ 1`, `289 ... differ 0`, `3908 ... differ 2`, san `0 failed` and `findings 0`, callgrind `Collected` within 1% of the number recorded at the previous stage (start: 3 891 585 598, run `ar2-t4`). Record the number.

## Review focus

Failure modes the spec implies that no old test pins; each has a test in the owning task.

1. Signed minimum by -1 at every width (`/` and `%`), including the narrow minima whose `0 - a` wraps back. Test: `selftest intarith` (T1), `trap_mod_min.c` (T2).
2. A shift count of exactly the width, one less, and a huge or negative count. Test: `selftest intarith` (T1), `int_ops.c` (T2), `tests/pp/int_ops.c` (T3).
3. `_Bool` canonicalization (`v != 0`) through the `cexpr` and `eval` adapters. Test: `selftest intarith` (T1); the existing goldens (T2).
4. Unsigned wrap at 8/16/32/64 bits and mixed signedness in `#if`. Test: `int_ops.c` (T2), `tests/pp/int_ops.c` (T3).
5. `pcexpr` against `ppexpr` on the same constants. Test: `tests/pc/int_ops.c` expected values equal gcc's (T3).

---

### Task 1: The kernel and its selftest

**Files:**
- Create: `src/intarith.h`, `src/intarith.c`
- Modify: `src/main.c` (dispatch next to `selftest punct`, line 1606), `tests/run.sh` (line 1058, after `selftest punct`)

**Interfaces:** produces `IaTy`, `IA_*`, `ia_canon`, `ia_is_arith`, `ia_binop`, `ia_shift_cpp`, `intarith_selftest`.

- [ ] **Step 1: Create `src/intarith.h`**

```c
#ifndef CEREAL_INTARITH_H
#define CEREAL_INTARITH_H
#include "token.h"

/* integer arithmetic in two's complement on at most 64 bits, shared by the constant folder
 * (c/cexpr.c), the evaluator (c/eval.c), #if (ppexpr.c) and presence conditions
 * (analysis/pcexpr.c).  The kernel never reports: it returns a defined value and flags
 * for what was undefined or lost, and each caller applies its own policy. */

/* a type: its width, signedness, and whether it is _Bool (canonical form v != 0) */
typedef struct { uint8_t bits; bool sg, boolean; } IaTy;
enum {
    IA_OVF = 1,          /* + - * / %: a signed result that does not fit (minimum / -1 and % -1 too) */
    IA_ZDIV = 2,         /* / % by zero (the value is 0) */
    IA_SHIFT_RANGE = 4,  /* count >= width (a negative count is huge) */
    IA_SHIFT_NEG = 8,    /* signed << of a negative value */
    IA_SHIFT_LOSSY = 16  /* signed <<: shifting the result back does not give the operand */
};

/* v as a value of t: truncated to its width, then sign- or zero-extended */
static inline uint64_t ia_canon(IaTy t, uint64_t v)
{
    unsigned sh;
    if (t.boolean)
        return v != 0;
    if (t.bits == 0 || t.bits >= 64)
        return v;
    sh = 64 - t.bits;
    return t.sg ? (uint64_t)((int64_t)(v << sh) >> sh) : (v << sh) >> sh;
}

/* the operators ia_binop computes */
static inline bool ia_is_arith(int op)
{
    return op == P_PLUS || op == P_MINUS || op == P_STAR || op == P_SLASH || op == P_PERCENT ||
           op == P_AMP || op == P_PIPE || op == P_CARET || op == P_SHL || op == P_SHR;
}

/* a OP b for canonical operands of t; the result is canonical, always defined */
static inline uint64_t ia_binop(int op, IaTy t, uint64_t a, uint64_t b, unsigned *flags)
{
    unsigned bits = t.bits == 0 || t.bits > 64 ? 64 : t.bits;
    bool sg = t.sg && !t.boolean;
    int64_t sa = (int64_t)a, sb = (int64_t)b, sr = 0;
    uint64_t r = 0;
    *flags = 0;
    switch (op) {
    case P_PLUS: case P_MINUS: case P_STAR: {
        bool o = op == P_PLUS ? __builtin_add_overflow(sa, sb, &sr)
                 : op == P_MINUS ? __builtin_sub_overflow(sa, sb, &sr)
                                 : __builtin_mul_overflow(sa, sb, &sr);
        r = op == P_PLUS ? a + b : op == P_MINUS ? a - b : a * b;
        if (sg && (o || ia_canon(t, (uint64_t)sr) != (uint64_t)sr))
            *flags |= IA_OVF;
        return ia_canon(t, r);
    }
    case P_SLASH: case P_PERCENT:
        if (b == 0) {
            *flags |= IA_ZDIV;
            return 0;
        }
        if (sg) {
            int64_t mn = bits == 64 ? INT64_MIN : -(INT64_C(1) << (bits - 1));
            if (sb == -1) {
                if (sa == mn)
                    *flags |= IA_OVF;
                r = op == P_SLASH ? 0 - a : 0;
            } else {
                r = (uint64_t)(op == P_SLASH ? sa / sb : sa % sb);
            }
        } else {
            r = op == P_SLASH ? a / b : a % b;
        }
        return ia_canon(t, r);
    case P_AMP: return ia_canon(t, a & b);
    case P_PIPE: return ia_canon(t, a | b);
    case P_CARET: return ia_canon(t, a ^ b);
    case P_SHL: case P_SHR:
        if (b >= bits) {
            *flags |= IA_SHIFT_RANGE;
            if (op == P_SHL && sg && a != 0)
                *flags |= IA_SHIFT_LOSSY;
            return op == P_SHR && sg && sa < 0 ? ~UINT64_C(0) : 0;
        }
        if (op == P_SHR)
            return ia_canon(t, sg ? (uint64_t)(sa >> b) : a >> b);
        r = ia_canon(t, a << b);
        if (sg) {
            if (sa < 0)
                *flags |= IA_SHIFT_NEG;
            if ((int64_t)r >> b != sa)
                *flags |= IA_SHIFT_LOSSY;
        }
        return r;
    default:
        return 0;
    }
}

/* #if and presence conditions: a negative count shifts the other way (libcpp); b_neg says the
 * count is a negative signed value, b its two's complement bits */
static inline uint64_t ia_shift_cpp(int op, IaTy t, uint64_t a, uint64_t b, bool b_neg, unsigned *flags)
{
    if (b_neg) {
        op = op == P_SHL ? P_SHR : P_SHL;
        b = 0 - b;
    }
    return ia_binop(op, t, a, b, flags);
}

int intarith_selftest(void);   /* `cereal selftest intarith`: the kernel against a wide reference */

#endif
```

- [ ] **Step 2: Create `src/intarith.c`**

```c
/* intarith.c - `cereal selftest intarith`: ia_binop and ia_canon against a 128-bit reference. */
#include <stdio.h>

#include "intarith.h"

__extension__ typedef __int128 Wide;
__extension__ typedef unsigned __int128 UWide;

static Wide t_lo(unsigned bits, bool sg) { return sg ? -((Wide)1 << (bits - 1)) : 0; }
static Wide t_hi(unsigned bits, bool sg)
{
    return sg ? ((Wide)1 << (bits - 1)) - 1 : ((Wide)1 << bits) - 1;
}
static bool t_fits(unsigned bits, bool sg, Wide v) { return v >= t_lo(bits, sg) && v <= t_hi(bits, sg); }

/* the canonical encoding of v modulo 2^bits */
static uint64_t t_wrap(unsigned bits, bool sg, Wide v)
{
    UWide m = bits == 64 ? (((UWide)1 << 64) - 1) : (((UWide)1 << bits) - 1);
    return ia_canon((IaTy){(uint8_t)bits, sg, false}, (uint64_t)((UWide)v & m));
}

static int t_fail(const char *what, unsigned bits, bool sg, Wide x, Wide y, uint64_t got, unsigned fl)
{
    fprintf(stderr, "intarith: %s bits=%u sg=%d x=%lld y=%lld got=%llx flags=%u\n", what, bits, sg,
            (long long)x, (long long)y, (unsigned long long)got, fl);
    return 1;
}

int intarith_selftest(void)
{
    static const unsigned widths[] = {8, 16, 32, 64};
    int bad = 0;
    unsigned wi, i, j, k;
    int sgi;
    /* canon: _Bool, and the width casts */
    if (ia_canon((IaTy){1, false, true}, 256) != 1 || ia_canon((IaTy){1, false, true}, 0) != 0 ||
        ia_canon((IaTy){8, true, false}, 0x80) != (uint64_t)-128 ||
        ia_canon((IaTy){8, false, false}, ~UINT64_C(0)) != 255 ||
        ia_canon((IaTy){32, true, false}, 0xffffffffu) != ~UINT64_C(0))
        bad += t_fail("canon", 0, 0, 0, 0, 0, 0);
    for (wi = 0; wi < 4; wi++)
        for (sgi = 0; sgi < 2; sgi++) {
            unsigned bits = widths[wi];
            bool sg = sgi;
            IaTy t = {(uint8_t)bits, sg, false};
            Wide v[48];
            unsigned n = 0;
            const Wide small[] = {0, 1, 2, 3, 7, bits - 1, bits, bits + 1};
            for (i = 0; i < 8; i++) {
                v[n++] = small[i];
                if (sg)
                    v[n++] = -small[i];
            }
            v[n++] = t_hi(bits, sg);
            v[n++] = t_lo(bits, sg);
            v[n++] = t_hi(bits, sg) - 1;
            v[n++] = t_lo(bits, sg) + 1;
            for (i = 0; i < n; i++)
                for (j = 0; j < n; j++) {
                    Wide x = v[i], y = v[j];
                    uint64_t a, b, r;
                    unsigned fl;
                    static const int arith[] = {P_PLUS, P_MINUS, P_STAR};
                    if (!t_fits(bits, sg, x) || !t_fits(bits, sg, y))
                        continue;
                    a = t_wrap(bits, sg, x);
                    b = t_wrap(bits, sg, y);
                    for (k = 0; k < 3; k++) {
                        Wide e = arith[k] == P_PLUS ? x + y : arith[k] == P_MINUS ? x - y : x * y;
                        r = ia_binop(arith[k], t, a, b, &fl);
                        if (r != t_wrap(bits, sg, e) || fl != (sg && !t_fits(bits, sg, e) ? IA_OVF : 0u))
                            bad += t_fail("+-*", bits, sg, x, y, r, fl);
                    }
                    if (y == 0) {
                        r = ia_binop(P_SLASH, t, a, b, &fl);
                        if (r != 0 || fl != IA_ZDIV)
                            bad += t_fail("zdiv", bits, sg, x, y, r, fl);
                    } else {
                        unsigned want = sg && y == -1 && x == t_lo(bits, sg) ? IA_OVF : 0u;
                        r = ia_binop(P_SLASH, t, a, b, &fl);
                        if (r != t_wrap(bits, sg, x / y) || fl != want)
                            bad += t_fail("/", bits, sg, x, y, r, fl);
                        r = ia_binop(P_PERCENT, t, a, b, &fl);
                        if (r != t_wrap(bits, sg, x % y) || fl != want)
                            bad += t_fail("%", bits, sg, x, y, r, fl);
                    }
                    if (ia_binop(P_AMP, t, a, b, &fl) != t_wrap(bits, sg, sg ? (Wide)((int64_t)a & (int64_t)b) : (Wide)(a & b)) || fl)
                        bad += t_fail("&", bits, sg, x, y, 0, fl);
                    if (y >= 0) {
                        r = ia_binop(P_SHR, t, a, b, &fl);
                        if (y >= bits ? fl != IA_SHIFT_RANGE || r != (sg && x < 0 ? ~UINT64_C(0) : 0)
                                      : fl || r != t_wrap(bits, sg, x >> (int)y))
                            bad += t_fail(">>", bits, sg, x, y, r, fl);
                        r = ia_binop(P_SHL, t, a, b, &fl);
                        if (y >= bits) {
                            if (fl != (IA_SHIFT_RANGE | (sg && x != 0 ? IA_SHIFT_LOSSY : 0u)) || r != 0)
                                bad += t_fail("<< range", bits, sg, x, y, r, fl);
                        } else {
                            Wide e = x * ((Wide)1 << (int)y);
                            unsigned want = (sg && x < 0 ? IA_SHIFT_NEG : 0u) |
                                            (sg && !t_fits(bits, sg, e) ? IA_SHIFT_LOSSY : 0u);
                            if (fl != want || r != t_wrap(bits, sg, e))
                                bad += t_fail("<<", bits, sg, x, y, r, fl);
                        }
                    }
                }
        }
    /* the libcpp rule: a negative count shifts the other way */
    {
        IaTy t = {64, true, false};
        unsigned fl;
        if (ia_shift_cpp(P_SHL, t, 8, (uint64_t)-1, true, &fl) != 4 || fl ||
            ia_shift_cpp(P_SHR, t, 8, (uint64_t)-2, true, &fl) != 32 || fl ||
            ia_shift_cpp(P_SHL, t, (uint64_t)-8, (uint64_t)-1, true, &fl) != (uint64_t)-4 || fl)
            bad += t_fail("shift_cpp", 64, 1, 0, 0, 0, fl);
    }
    if (bad)
        fprintf(stderr, "intarith: %d failures\n", bad);
    return bad != 0;
}
```

- [ ] **Step 3: Dispatch and run it**

In `src/main.c`, after the `punct` dispatch (it needs `#include "intarith.h"` next to the other includes):

```c
    if (argc == 3 && !strcmp(argv[1], "selftest") && !strcmp(argv[2], "intarith"))
        return intarith_selftest();
```

In `tests/run.sh` after the `selftest punct` line:

```sh
if "$CEREAL" selftest intarith </dev/null; then ok; else bad "selftest intarith"; fi
```

- [ ] **Step 4: Run it, then show it can fail**

Run: `MSYS_NO_PATHCONV=1 wsl -e bash -c 'cd ~/cereal-s1 && sh .sync.sh >/dev/null 2>&1; make -j4 2>&1 | grep -E "error|warning"; ./cereal selftest intarith </dev/null; echo rc=$?'`
Expected: no compiler output, `rc=0`. Then change `0 - a : 0` to `0 - a : 1` in the `%` branch of `ia_binop`, rebuild: expected `intarith: ... failures` and `rc=1`. Revert the change.

- [ ] **Step 5: Commit** (no full gate: nothing calls the kernel yet; the quick loop must end `N passed, 0 failed` with N one higher than before)

```bash
git add src/intarith.h src/intarith.c src/main.c tests/run.sh && git commit -m "AR3 T1: integer-op kernel (intarith.h), selftest intarith

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 2: `cexpr` and `eval` use the kernel (stage 1)

**Files:**
- Create: `tests/eval/int_ops.c`, `tests/eval/trap_mod_min.c`
- Modify: `src/c/cexpr.c` (`cexpr_trunc` line 118, `int_op` 174-259, callers 6245 and 6525), `src/c/eval.c` (`ev_canon` 75, `fits` 605 stays, `int_arith` 610-668)

**Interfaces:** consumes T1. Produces nothing new.

- [ ] **Step 1: The characterization test first (`tests/eval/int_ops.c`)**

```c
/* + - * / % & | ^ << >> on 8-, 16-, 32- and 64-bit integers, signed (defined cases only) and
 * unsigned, run and folded; eval.sh compares the evaluator with gcc */
#include <stdio.h>
#include <stdint.h>

#define H(h, v) ((h) * 1000003ULL + (unsigned long long)(v))

#define UOPS(N, T, W)                                                              \
    static unsigned long long u##N(T a, T b)                                       \
    {                                                                              \
        unsigned long long h = 7;                                                  \
        unsigned k = (unsigned)(b % W);                                            \
        h = H(h, (T)(a + b)); h = H(h, (T)(a - b)); h = H(h, (T)((uint64_t)a * b)); \
        if (b) { h = H(h, (T)(a / b)); h = H(h, (T)(a % b)); }                     \
        h = H(h, (T)(a & b)); h = H(h, (T)(a | b)); h = H(h, (T)(a ^ b));          \
        h = H(h, (T)(a >> k)); h = H(h, (T)((uint64_t)a << k));                    \
        return h;                                                                  \
    }

/* signed: every operation is done wide first and kept only when it fits T */
#define SOPS(N, T, W, LO)                                                            \
    static unsigned long long s##N(T a, T b)                                       \
    {                                                                              \
        unsigned long long h = 11;                                                 \
        long long x = a, y = b, lo = (LO), hi = -(lo + 1);                          \
        unsigned k = (unsigned)((y < 0 ? -y : y) % W);                             \
        if (x + y >= lo && x + y <= hi) h = H(h, (T)(a + b));                      \
        if (x - y >= lo && x - y <= hi) h = H(h, (T)(a - b));                      \
        if (W < 64 && x * y >= lo && x * y <= hi) h = H(h, (T)(a * b));            \
        if (b != 0 && !(b == -1 && x == lo)) { h = H(h, (T)(a / b)); h = H(h, (T)(a % b)); } \
        h = H(h, (T)(a & b)); h = H(h, (T)(a | b)); h = H(h, (T)(a ^ b));          \
        h = H(h, (T)(a >> k));                                                     \
        if (a >= 0 && W < 64 && (x << k) <= hi) h = H(h, (T)(a << k));             \
        return h;                                                                  \
    }

UOPS(8, uint8_t, 8)
UOPS(16, uint16_t, 16)
UOPS(32, uint32_t, 32)
UOPS(64, uint64_t, 64)
SOPS(8, int8_t, 8, -128)
SOPS(16, int16_t, 16, -32768)
SOPS(32, int32_t, 32, -2147483647LL - 1)
SOPS(64, int64_t, 64, -9223372036854775807LL - 1)

int main(void)
{
    static const long long v[] = {0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 63, 64, 100, 127, 128, 255,
                                  256, 1000, 32767, 32768, 65535, 65536, 2147483647LL,
                                  2147483648LL, 4294967295LL, 4294967296LL, -1, -2, -3, -7,
                                  -8, -100, -127, -128, -129, -255, -32768, -32769, -65536,
                                  -2147483648LL, -2147483649LL};
    unsigned long long h = 0;
    unsigned i, j;
    for (i = 0; i < sizeof v / sizeof v[0]; i++)
        for (j = 0; j < sizeof v / sizeof v[0]; j++) {
            long long x = v[i], y = v[j];
            h = H(h, u8((uint8_t)x, (uint8_t)y)); h = H(h, u16((uint16_t)x, (uint16_t)y));
            h = H(h, u32((uint32_t)x, (uint32_t)y)); h = H(h, u64((uint64_t)x, (uint64_t)y));
            h = H(h, s8((int8_t)x, (int8_t)y)); h = H(h, s16((int16_t)x, (int16_t)y));
            h = H(h, s32((int32_t)x, (int32_t)y)); h = H(h, s64(x, y));
        }
    printf("%llu\n", h);
    printf("%d %u %d %d\n", 7 / 2 - 8 % 3, 1u << 31, -17 >> 2, (int)(0x7fffffff + 0u));
    return (int)(h & 127);
}
```

- [ ] **Step 2: Run it against the current build**

Run: `MSYS_NO_PATHCONV=1 wsl -e bash -c 'cd ~/cereal-s1 && sh .sync.sh >/dev/null 2>&1; sh tests/eval.sh ./cereal 2>&1 | tail -3'`
Expected: `eval: 54 passed, 0 failed` (53 before; the new case passes before any change: it is a characterization against gcc).

- [ ] **Step 3: The trap test for decision p1 (`tests/eval/trap_mod_min.c`)**

```c
// trap: signed integer overflow
int main(void) { int a = -2147483647 - 1, b = -1; return a % b; }
```

Run `sh tests/eval.sh ./cereal`: expected one failure, `FAIL trap_mod_min` (the evaluator returns 0 today). That failure is the decided behavior change, fixed by step 5.

- [ ] **Step 4: `cexpr.c`**

Add the include for `intarith.h` in the form the file uses for its sibling headers. Add above `cexpr_trunc` a type-table adapter and make `cexpr_trunc` use it:

```c
static IaTy ia_ty(Checker *c, TypeId t)
{
    unsigned bits = int_bits(c, t);
    return (IaTy){(uint8_t)(bits == 0 || bits > 64 ? 64 : bits), is_signed(c, t), tkind(c, t) == TY_BOOL};
}
```

`cexpr_trunc` keeps its non-integer pass-through and becomes

```c
uint64_t cexpr_trunc(Checker *c, TypeId t, uint64_t v)
{
    if (tkind(c, t) != TY_BOOL && !is_int(c, t) && !is_ptr(c, t))
        return v;
    return ia_canon(ia_ty(c, t), v);
}
```

(The old code did not sign-extend a pointer: check that `is_signed(c, ptr)` is false, and if it is not, pass `.sg = is_int(c, t) && is_signed(c, t)`.) Delete `int_op` (lines 172-259 with its comment). The two callers:

```c
    {
        unsigned fl;
        uint64_t v = ia_binop(op, ia_ty(c, ta), cexpr_trunc(c, ta, c->cv[a]), c->cv[b], &fl);
        bin_value(c, i, a, b, v, false, int_const, loc);
    }
```

and

```c
            unsigned fl;
            uint64_t v = ia_binop(op, ia_ty(c, rt), cexpr_trunc(c, rt, c->cv[a]),
                                  cexpr_trunc(c, rt, c->cv[b]), &fl);
            bin_value(c, i, a, b, v, fl & IA_OVF, true, loc);
```

Read both callers' surrounding lines first. The old second caller also had a `zd` out-parameter; if the code after the call uses it, pass `fl & IA_ZDIV` (the divide-by-zero case is checked before the call through `zero_div`, so it should be unused). If the operator can be one `ia_binop` does not compute (`ia_is_arith(op)` false), keep the old meaning (`r = 0`) by guarding the call, and say so in the commit.

- [ ] **Step 5: `eval.c`**

Add the include. Replace `ev_canon`'s body, keep `fits` but define it over the kernel (it has a user outside `int_arith`: the unary minus at about line 1820), and write `int_arith`:

```c
static IaTy ev_iaty(const Ti *ti)
{
    return (IaTy){(uint8_t)(ti->bytes >= 8 ? 64 : 8 * ti->bytes), ti->sg, ti->boolean};
}

uint64_t ev_canon(const Ti *ti, uint64_t x)
{
    return ia_canon(ev_iaty(ti), x);
}

static bool fits(const Ti *ti, int64_t x)
{
    return ti->bytes >= 8 || (int64_t)ev_canon(ti, (uint64_t)x) == x;
}
```

(`fits` keeps its place before `int_arith`'s old position; delete only its old definition, and `int_arith` no longer calls it.)

```c
static uint64_t int_arith(Ev *v, int p, TypeId ty, uint64_t a, uint64_t b)
{
    Ti ti;
    unsigned fl;
    uint64_t r;
    if (!ev_tinfo(v, ty, &ti))
        return 0;
    if (!ia_is_arith(p)) {
        UNSUP(v, "binary operator %d", p);
        return 0;
    }
    r = ia_binop(p, ev_iaty(&ti), a, b, &fl);
    if (fl & IA_ZDIV) {
        TRAP(v, "division by zero");
        return 0;
    }
    if (fl & IA_SHIFT_RANGE) {
        TRAP(v, "shift count out of range");
        return 0;
    }
    if ((fl & IA_OVF) && !v->o.wrap) {
        TRAP(v, "signed integer overflow");
        return 0;
    }
    if ((fl & IA_SHIFT_NEG) && !v->o.wrap) {
        TRAP(v, "left shift of a negative value");
        return 0;
    }
    return r;
}
```

Checked against the old function: `ti.sg && !ti.boolean` guarded the overflow traps (the kernel's `sg && !boolean` does it); old `/` with `sb == -1` returned `ev_canon(0 - a)` (same value); old shifts trapped on `b >= 8 * ti.bytes`.

- [ ] **Step 6: Verify**

Run: `cd src && grep -n "int_op\|int_arith\|[^_]fits(" c/cexpr.c c/eval.c`
Expected: only `int_arith` (definition and its one caller) and `fits` (definition and the unary-minus use). Then the quick loop (it includes `tests/eval.sh`: 55 cases now, `trap_mod_min` passing) and the full gate with `-Name ar3-t2`. Expected as in the recipe; record the callgrind number against 3 891 585 598 (within 1%).

- [ ] **Step 7: Commit**

```bash
git add src tests && git commit -m "AR3 T2: cexpr and eval use intarith.h; INT_MIN % -1 traps

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 3: `ppexpr` and `pcexpr` use the kernel (stage 2)

**Files:**
- Create: `tests/pp/int_ops.c`, `tests/pc/int_ops.c`, `tests/pc/int_ops.pc`
- Modify: `src/ppexpr.c` (`mul`, `add`, `shift`, about 497-625), `src/analysis/pcexpr.c` (`fold`, about 664-705)

**Interfaces:** consumes T1.

- [ ] **Step 1: The `#if` test first (`tests/pp/int_ops.c`)**

`tests/pp/*.c` are compared with `gcc -E` by `diff_pp`, so the expected output is gcc's. One `#if` per case: arithmetic and shifts on signed and unsigned, negative counts, counts of 63, 64 and 70, `-1 >> 100`, the minimum by -1, unsigned wrap, `%` by -1:

```c
/* integer arithmetic and shifts in #if; diff_pp compares with gcc -E */
#if 8 << -1 == 4
a1
#endif
#if (8 >> -2) == 32
a2
#endif
#if -1 >> 100 == -1
a3
#endif
#if (1 << 63) < 0
a4
#endif
#if (1u << 63) > 0
a5
#endif
#if (-8 << 1) == -16
a6
#endif
#if (1 << 64) == 0
a7
#endif
#if (1 << 70) == 0
a8
#endif
#if ((-9223372036854775807 - 1) % -1) == 0
a9
#endif
#if (0u - 1) == 18446744073709551615u
a10
#endif
#if (-7 / 2) == -3 && (-7 % 2) == -1 && (7u / 2) == 3
a11
#endif
#if (6 & 3) == 2 && (6 | 3) == 7 && (6 ^ 3) == 5
a12
#endif
#if (-1 >> 1) == -1 && (-2 >> 1) == -1
a13
#endif
#if (3000000000 * 3) / 3 == 3000000000
a14
#endif
```

Write it, run `gcc -E` on it and read the output: every `aN` gcc keeps is a case that must also hold in `cereal -E`. gcc's `integer overflow in preprocessor expression` warnings are not errors, and `diff_pp` compares the preprocessed text. If gcc rejects a line, drop that line and note why in the commit.

- [ ] **Step 2: The presence-condition test (`tests/pc/int_ops.c`, `.pc`)**

The same `#if` lines (the `aN` text lines may stay). Run

`CEREAL_PCDUMP=1 ./cereal lint tests/pc/int_ops.c 2>&1 >/dev/null </dev/null | grep '^pc:'`

and save the output as `tests/pc/int_ops.pc` only after checking each line's `host=` against the `gcc -E` result of step 1 (`host=1` where gcc keeps the text). Before the change `pcexpr` disagrees with gcc on `8 << -1`, `-1 >> 100` and `8 >> -2`; edit those lines' `host=` to gcc's values by hand: they are the decided stage 2 behavior change, so the test fails until step 4.

- [ ] **Step 3: `ppexpr.c`**

Add the include. One helper for all three functions: `static IaTy pp_ty(Val a) { return (IaTy){64, !a.uns, false}; }`. In `mul`, replace the `if (op->punct == P_STAR) {...} else {...}` body by

```c
        {
            unsigned fl;
            uint64_t r = ia_binop(op->punct, pp_ty(a), a.v, b.v, &fl);
            if (fl & IA_ZDIV) {
                if (eval)
                    fail(p, op, "division by zero in #if", NULL, 0);
                a.v = 0;
                continue;
            }
            if (fl & IA_OVF)
                overflow(p, op, eval);
            a.v = r;
        }
```

In `add`, the same with `P_PLUS`/`P_MINUS` (no `ZDIV`). In `shift`, replace everything between `a.loc = op->loc;` and the end of the loop body by

```c
        {
            unsigned fl;
            bool bneg = !b.uns && sv(b) < 0;
            a.v = ia_shift_cpp(op->punct, pp_ty(a), a.v, b.v, bneg, &fl);
            if (fl & IA_SHIFT_LOSSY)
                overflow(p, op, eval);
        }
```

Read the old `shift` first: it clamps an unsigned count to 64 (`ia_binop` treats any count of at least the width as out of range, so the clamp goes) and flips the direction for a negative count (`ia_shift_cpp` does). Delete the `INTMAX`/`UINTMAX` arithmetic that `ia_binop` replaced.

- [ ] **Step 4: `pcexpr.c`**

Add the include. In `fold`, replace the cases `P_STAR P_SLASH P_PERCENT P_PLUS P_MINUS P_SHL P_SHR P_AMP P_CARET P_PIPE` by

```c
    case P_STAR: case P_SLASH: case P_PERCENT: case P_PLUS: case P_MINUS:
    case P_AMP: case P_CARET: case P_PIPE: {
        unsigned fl;
        r = (intmax_t)ia_binop(op, (IaTy){64, !uns, false}, ux, uy, &fl);
        if (fl & IA_ZDIV)
            return false;
        break;
    }
    case P_SHL: case P_SHR: {
        unsigned fl;
        r = (intmax_t)ia_shift_cpp(op, (IaTy){64, !x->u, false}, ux, uy, !y->u && y->n < 0, &fl);
        ru = x->u;
        break;
    }
```

(`uns` is `x->u || y->u` as before; `ru = uns` stays the default for the first group. Keep the relation cases from AR2 and the `default: return false`.)

- [ ] **Step 5: Verify**

Run the quick loop: `tests/pp/int_ops.c` (twice: default and gnu99) and `tests/pc/int_ops` pass. Then `cd src && grep -n "case P_SLASH" -r .`: expected only `intarith.h`, the float code (`fbin` in `cexpr.c` and `eval.c`), and the non-value switches in `cgx86.c`, `tast.c`, `cseqpt.c`, `cwarn_expr.c`; list what remains in the commit message. Full gate with `-Name ar3-t3`, callgrind within 1% of T2's.

- [ ] **Step 6: Commit**

```bash
git add src tests && git commit -m "AR3 T3: ppexpr and pcexpr use intarith.h; pcexpr shifts follow libcpp

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Docs and close

**Files:**
- Modify: `docs/CLEANUP_PLAN.md` (AR3 item), `docs/AR3_DESIGN.md` (status, amended text), `docs/AR3_PLAN.md` (status)
- Check: `docs/ARCHITECTURE.md`, `docs/PARSER.md`, `docs/TYPES.md`, `docs/HISTORY.md`, `docs/STATUS.md`, `PARKED.md`, `ROADMAP.md`

- [ ] **Step 1: Find stale wording**

Run: `grep -rn "int_op\b\|int_arith\|cexpr_trunc\|ev_canon" docs *.md | grep -v "^docs/CLEANUP_PLAN.md\|^docs/AR[23]_"`
For each hit: dated history stays; text describing live code is edited to name `intarith.h`.

- [ ] **Step 2: Amend the spec**

In `docs/AR3_DESIGN.md`: set `status: done`; in the flags text and the per-site table make `IA_SHIFT_LOSSY` also cover the out-of-range left shift of a nonzero signed operand and say `ppexpr` tests it alone; name the deleted code (`int_op`, `int_arith`, `fits`, the arithmetic bodies of `mul`/`add`/`shift`, `fold`'s integer cases) and the two behavior changes made.

- [ ] **Step 3: Record the outcome**

Append to the AR3 item in `docs/CLEANUP_PLAN.md`: `**Done 2026-MM-DD** (commits): ...` with the commits of T1-T3, lines removed (`git diff --stat <T1 parent>..HEAD`), the gate run ids, the callgrind numbers, the two behavior changes (`INT_MIN % -1` traps in `eval`; `pcexpr` shifts), and `Not done:` whatever remained (floating point, unary operators).

- [ ] **Step 4: Plan status and commit**

Set `status: done` in `docs/AR3_PLAN.md`.

```bash
git add docs && git commit -m "AR3: close the plan; docs name intarith.h

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

## Self-review

- **Spec coverage:** kernel and flags (T1), `cexpr`/`eval` and p1 (T2), `ppexpr`/`pcexpr` and p2 (T3), canonicalizer adapters (T2), selftest and the test files, gates, docs (T4). The spec's `ia_is_arith` and `ia_shift_cpp` are in T1.
- **Placeholders:** none; the places that say "read the lines" name what to read and the rule to apply (`cexpr` callers, `eval` `fits` users, the `shift` clamp).
- **Type consistency:** `IaTy{bits, sg, boolean}`; `ia_binop(op, t, a, b, &flags)`; `ia_shift_cpp(op, t, a, b, b_neg, &flags)`; flags `IA_OVF IA_ZDIV IA_SHIFT_RANGE IA_SHIFT_NEG IA_SHIFT_LOSSY`; callers use only these.

## Stress rounds

Round 2 (2026-10-31): grepped the plan against the source: `fits` has a third user (`eval.c:1820`, unary minus), so it stays (T2 step 5 and 6 corrected); the `ia_`/`IA_` names do not collide with the intrinsic table's `IA_*` macros in `cbuiltin_x86.h` (`IA_SHIFTS`, `IA_V4SI`, ... are different tokens); `fold`'s variable names, `ppexpr`'s `Val.uns`/`.v` and the `continue` inside `mul`'s loop match the text. Nothing else material.

Round 1 (2026-10-31, before handing over): ran the kernel and the selftest in scratch (clean under the project flags, and the seeded wrong rule fails); ran `int_ops.c` under UBSan (found and removed signed-shift UB in the test's bounds), then through the current `cereal run --eval-wrap` (identical to gcc); found that a range `<<` must also set `IA_SHIFT_LOSSY` for `ppexpr`'s overflow-on-range to be one flag, and recorded the spec amendment here. Not yet checked: the `tests/pp/int_ops.c` lines against gcc (T3 step 1 does it), and `is_signed` on pointer types (T2 step 4 says to check).
