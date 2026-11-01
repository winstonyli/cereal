---
status: draft
---

# AR2 implementation plan: operator-property table and `rel_holds`

> **For agentic workers:** execute inline, task by task (`~/.claude/WORKING_STYLE.md`: no subagents for a plan of this shape). Steps use checkbox syntax.

**Goal:** the properties of a punctuator and the relation of two compared values each have one home; about 14 copied helpers, three precedence tables and nine value switches go.

**Architecture:** a const `punct_info[P_COUNT]` table in `lex.c` beside `punct_spelling`, static-inline accessors and `rel_holds`/`rel_cmp_*` in `token.h`, a hidden `selftest punct`. Four stages, each a commit with a green gate.

**Tech stack:** C, `sh` gate in WSL (`~/cereal-s1`), Windows tree is the source of truth.

**Spec:** `docs/AR2_DESIGN.md` (amended by this plan: `punct_assign_base` returns the operator itself when it is not a compound assignment, because `binop_p` in `cconv.c` relies on that; `cmp_str` stays, since `cwarn_expr.c` passes it non-comparison operators).

## Global constraints

- Edit with the Edit tool. No `python -` on stdin; a script goes in a file and runs with `python3 -P f.py </dev/null`. Python writes with `newline=''`; after any script or `git checkout`, `sed -i 's/\r$//'` the touched files.
- No compatibility wrappers: when a helper goes, every caller is changed in the same commit.
- Commit trailer: `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`. Never push.
- The Windows tree is edited; WSL gets it by `sh .sync.sh` in `~/cereal-s1` (rsync; it does not delete files removed on the Windows side).
- Results must not change: goldens, verify (3908/3910 identical, 637/638, 289/289) and san findings 0 stay as they are.

## Gate recipe (used at the end of every task)

Quick loop (6-10 min), from Git Bash:

```bash
MSYS_NO_PATHCONV=1 wsl -e bash -c 'cd ~/cereal-s1 && sh .sync.sh >/dev/null 2>&1; make -j4 2>&1 | grep -E "error|warning"; GOLDEN_JOBS=6 PATH=$HOME/tools/z3:$PATH EVAL_SWEEP_SOLVE="z3 -in -smt2" sh tests/run.sh 2>&1 | tail -3'
```

Expected: no compiler output, last line `N passed, 0 failed`.

Full gate (about 15 min), detached, then a watcher with a deadline:

```bash
cd "/c/Users/Winston Li/Documents/GitHub/cereal" && powershell -NoProfile -File "$USERPROFILE/.claude/runs/run.ps1" -Kind gate -Name ar2-tN -Detach -Command "wsl -e bash /home/winstonyli/gate-run.sh" | tail -1
d="$LOCALAPPDATA/claude-runs/cereal/$(date +%F)-ar2-tN"; for i in $(seq 1 55); do grep -q '"end":  null' "$d/run.json" || break; sleep 10; done; grep -E '"end"|"exit"' "$d/run.json"; head -20 "$d/out.log"
```

(Repeat the watcher if it times out first.) Expected in `out.log`: `globals: ... all allowed`, run `N passed, 0 failed`, verify `637 ... differ 1`, `289 ... differ 0`, `3908 ... differ 2`, san `0 failed` and `findings 0`, callgrind `Collected` within about 1% of 3 890 870 693 (record the number).

## Review focus

Failure modes the spec implies that no task's old tests pin; each has a test in the owning task.

1. A NaN operand: only `!=` holds. Test: `selftest punct` (T1), `tests/eval/rel_ops.c` (T4).
2. The same bits compared signed and unsigned (`-1` against `1`). Test: `rel_ops.c` (T4).
3. Pointers into one array compared at compile time (`cwarn_expr.c` address folding). Test: `rel_ops.c` (T4).
4. Equal-precedence neighbours (`==` with `!=`, `<` with `>=`, `<<` with `>>`) and the comma at level 0. Test: `selftest punct` (T1).
5. A compound assignment's base operator (every `X=` maps to `X`). Test: `selftest punct` compares spellings (T1).

---

### Task 1: Table, accessors, `rel_holds`, `selftest punct`

**Files:**
- Modify: `src/token.h` (new declarations after `punct_spelling`, line 53; `PR_*` enum moves here in T2)
- Modify: `src/lex.c` (table and selftest after `punct_spelling`, line 19-24)
- Modify: `src/main.c:1602-1603` (dispatch), `tests/run.sh:1056` (run the selftest)

**Interfaces:**
- Produces: `PunctInfo`, `punct_info`, `punct_prec`, `punct_is_cmp`, `punct_is_assign`, `punct_is_logical`, `punct_is_bool`, `punct_swap`, `punct_negate`, `punct_assign_base`, `tok_prec`, `REL_UNORDERED`, `rel_holds`, `rel_cmp_s`, `rel_cmp_u`, `rel_cmp_f`, `punct_selftest`, `PR_*` constants.

- [ ] **Step 1: Check the names are free**

Run: `grep -rnw "punct_info\|punct_prec\|punct_swap\|punct_negate\|rel_holds\|rel_cmp_s\|REL_UNORDERED\|tok_prec\|PunctInfo" src tests | head`
Expected: no output.

- [ ] **Step 2: Move the precedence enum into `token.h`**

In `src/c/cexpr_int.h:14-17` delete

```c
enum {
    PR_COMMA, PR_ASSIGN, PR_COND, PR_LOR, PR_LAND, PR_OR, PR_XOR, PR_AND,
    PR_EQ, PR_REL, PR_SHIFT, PR_ADD, PR_MUL, PR_UNARY, PR_POSTFIX,
    PR_PRIMARY
};
```

and put it in `src/token.h` directly after the `Punct` typedef (after line 51) with a comment line above it: `/* C's expression precedence, lowest first; a binary operator's level is punct_info[op].prec (PR_COMMA, 0, means "not a binary operator" to the Pratt parsers). */`.

- [ ] **Step 3: Declarations in `token.h`**

After `extern const char *const punct_spelling[P_COUNT];` add:

```c
enum { PF_CMP = 1, PF_ASSIGN = 2, PF_LOGICAL = 4 };

typedef struct {
    uint8_t prec;    /* binary precedence (PR_*); 0: not a binary operator */
    uint8_t flags;   /* PF_* */
    uint8_t swap;    /* a OP b == b swap(OP) a; 0: the operator itself */
    uint8_t negate;  /* !(a OP b) == a negate(OP) b; 0: none (valid for ordered operands only) */
    uint8_t base;    /* the binary operator of a compound assignment; 0: the operator itself */
} PunctInfo;

extern const PunctInfo punct_info[P_COUNT];

static inline const PunctInfo *punct_ent(int op)
{
    return &punct_info[(unsigned)op < P_COUNT ? op : P_NONE];
}
static inline int punct_prec(int op) { return punct_ent(op)->prec; }
static inline bool punct_is_cmp(int op) { return punct_ent(op)->flags & PF_CMP; }
static inline bool punct_is_assign(int op) { return punct_ent(op)->flags & PF_ASSIGN; }
static inline bool punct_is_logical(int op) { return punct_ent(op)->flags & PF_LOGICAL; }
/* the value is 0 or 1: a comparison, && or || */
static inline bool punct_is_bool(int op) { return punct_ent(op)->flags & (PF_CMP | PF_LOGICAL); }
static inline int punct_swap(int op) { return punct_ent(op)->swap ? punct_ent(op)->swap : op; }
static inline int punct_negate(int op) { return punct_ent(op)->negate; }
static inline int punct_assign_base(int op) { return punct_ent(op)->base ? punct_ent(op)->base : op; }

/* `a OP b` for a comparison OP, from cmp = rel_cmp_*(a, b): -1, 0, 1, or
 * REL_UNORDERED (a NaN operand: only != holds). */
enum { REL_UNORDERED = 2 };
static inline bool rel_holds(int op, int cmp)
{
    switch (op) {
    case P_EQEQ: return cmp == 0;
    case P_NE: return cmp != 0;
    case P_LT: return cmp == -1;
    case P_GT: return cmp == 1;
    case P_LE: return cmp == -1 || cmp == 0;
    default: return cmp == 0 || cmp == 1;        /* P_GE */
    }
}
static inline int rel_cmp_s(int64_t a, int64_t b) { return (a > b) - (a < b); }
static inline int rel_cmp_u(uint64_t a, uint64_t b) { return (a > b) - (a < b); }
static inline int rel_cmp_f(long double a, long double b)
{
    return a < b ? -1 : a > b ? 1 : a == b ? 0 : REL_UNORDERED;
}

int punct_selftest(void);   /* `cereal selftest punct`: the table against first principles */
```

and, after the `Tok` typedef (find it with `grep -n "} Tok;" src/token.h`):

```c
/* the binary precedence of a token, 0 when it is not a binary operator */
static inline int tok_prec(const Tok *t) { return t->kind == TK_PUNCT ? punct_prec(t->punct) : 0; }
```

`token.h` includes only `<string.h>` now: add `#include <stdbool.h>` and `#include <stdint.h>` beside it.

- [ ] **Step 4: Write the selftest first (in `lex.c`, after the `punct_spelling` array)**

```c
/* every field is given: -Wextra warns on a partial initialiser */
#define PI(prec, flags, swap, negate, base) {prec, flags, swap, negate, base}
const PunctInfo punct_info[P_COUNT] = {
    [P_STAR] = PI(PR_MUL, 0, 0, 0, 0), [P_SLASH] = PI(PR_MUL, 0, 0, 0, 0),
    [P_PERCENT] = PI(PR_MUL, 0, 0, 0, 0),
    [P_PLUS] = PI(PR_ADD, 0, 0, 0, 0), [P_MINUS] = PI(PR_ADD, 0, 0, 0, 0),
    [P_SHL] = PI(PR_SHIFT, 0, 0, 0, 0), [P_SHR] = PI(PR_SHIFT, 0, 0, 0, 0),
    [P_LT] = PI(PR_REL, PF_CMP, P_GT, P_GE, 0),
    [P_GT] = PI(PR_REL, PF_CMP, P_LT, P_LE, 0),
    [P_LE] = PI(PR_REL, PF_CMP, P_GE, P_GT, 0),
    [P_GE] = PI(PR_REL, PF_CMP, P_LE, P_LT, 0),
    [P_EQEQ] = PI(PR_EQ, PF_CMP, 0, P_NE, 0),
    [P_NE] = PI(PR_EQ, PF_CMP, 0, P_EQEQ, 0),
    [P_AMP] = PI(PR_AND, 0, 0, 0, 0), [P_CARET] = PI(PR_XOR, 0, 0, 0, 0),
    [P_PIPE] = PI(PR_OR, 0, 0, 0, 0),
    [P_ANDAND] = PI(PR_LAND, PF_LOGICAL, 0, 0, 0),
    [P_OROR] = PI(PR_LOR, PF_LOGICAL, 0, 0, 0),
    [P_ASSIGN] = PI(0, PF_ASSIGN, 0, 0, 0),
    [P_MUL_ASSIGN] = PI(0, PF_ASSIGN, 0, 0, P_STAR),
    [P_DIV_ASSIGN] = PI(0, PF_ASSIGN, 0, 0, P_SLASH),
    [P_MOD_ASSIGN] = PI(0, PF_ASSIGN, 0, 0, P_PERCENT),
    [P_ADD_ASSIGN] = PI(0, PF_ASSIGN, 0, 0, P_PLUS),
    [P_SUB_ASSIGN] = PI(0, PF_ASSIGN, 0, 0, P_MINUS),
    [P_SHL_ASSIGN] = PI(0, PF_ASSIGN, 0, 0, P_SHL),
    [P_SHR_ASSIGN] = PI(0, PF_ASSIGN, 0, 0, P_SHR),
    [P_AND_ASSIGN] = PI(0, PF_ASSIGN, 0, 0, P_AMP),
    [P_XOR_ASSIGN] = PI(0, PF_ASSIGN, 0, 0, P_CARET),
    [P_OR_ASSIGN] = PI(0, PF_ASSIGN, 0, 0, P_PIPE),
};
#undef PI
_Static_assert(P_COUNT < 256, "PunctInfo holds operators in a byte");

#define SELF_REL(cmpf, a, b)                                                   \
    do {                                                                       \
        int c_ = cmpf((a), (b));                                               \
        if (rel_holds(P_EQEQ, c_) != ((a) == (b))) bad++;                      \
        if (rel_holds(P_NE, c_) != ((a) != (b))) bad++;                        \
        if (rel_holds(P_LT, c_) != ((a) < (b))) bad++;                         \
        if (rel_holds(P_GT, c_) != ((a) > (b))) bad++;                         \
        if (rel_holds(P_LE, c_) != ((a) <= (b))) bad++;                        \
        if (rel_holds(P_GE, c_) != ((a) >= (b))) bad++;                        \
        /* a OP b is b swap(OP) a, ordered or not */                           \
        for (k = 0; k < 6; k++)                                                \
            if (rel_holds(cmps[k], c_) !=                                      \
                rel_holds(punct_swap(cmps[k]), cmpf((b), (a))))                \
                bad++;                                                         \
    } while (0)

int punct_selftest(void)
{
    static const int cmps[6] = {P_EQEQ, P_NE, P_LT, P_GT, P_LE, P_GE};
    /* strictly increasing precedence; operators of one level are listed in groups */
    static const int levels[][4] = {
        {P_OROR}, {P_ANDAND}, {P_PIPE}, {P_CARET}, {P_AMP}, {P_EQEQ, P_NE},
        {P_LT, P_GT, P_LE, P_GE}, {P_SHL, P_SHR}, {P_PLUS, P_MINUS},
        {P_STAR, P_SLASH, P_PERCENT}};
    static const int64_t si[] = {-2, -1, 0, 1, 2, INT64_MIN, INT64_MAX};
    static const uint64_t ui[] = {0, 1, 2, UINT64_MAX, (uint64_t)1 << 63};
    static const long double fv[] = {-1.5L, 0.0L, 1.5L, 1e300L, __builtin_nanl("")};
    int bad = 0, i, j, k, l;
    for (i = 0; i < (int)(sizeof si / sizeof *si); i++)
        for (j = 0; j < (int)(sizeof si / sizeof *si); j++)
            SELF_REL(rel_cmp_s, si[i], si[j]);
    for (i = 0; i < (int)(sizeof ui / sizeof *ui); i++)
        for (j = 0; j < (int)(sizeof ui / sizeof *ui); j++)
            SELF_REL(rel_cmp_u, ui[i], ui[j]);
    for (i = 0; i < (int)(sizeof fv / sizeof *fv); i++)
        for (j = 0; j < (int)(sizeof fv / sizeof *fv); j++)
            SELF_REL(rel_cmp_f, fv[i], fv[j]);
    for (k = 0; k < 6; k++) {
        int o = cmps[k];
        if (!punct_is_cmp(o) || !punct_is_bool(o) || punct_is_logical(o) || punct_is_assign(o))
            bad++;
        if (punct_swap(punct_swap(o)) != o || punct_negate(punct_negate(o)) != o)
            bad++;
        if (punct_negate(punct_swap(o)) != punct_swap(punct_negate(o)))
            bad++;
        for (i = -1; i <= 1; i++)            /* ordered operands: negation is the complement */
            if (rel_holds(punct_negate(o), i) == rel_holds(o, i))
                bad++;
        if (rel_holds(o, REL_UNORDERED) != (o == P_NE))
            bad++;
    }
    for (i = 0; i < (int)(sizeof levels / sizeof *levels); i++)
        for (j = 0; j < 4 && levels[i][j]; j++) {
            if (punct_prec(levels[i][j]) != punct_prec(levels[i][0]) || punct_prec(levels[i][j]) < PR_LOR)
                bad++;
            if (i && punct_prec(levels[i][j]) <= punct_prec(levels[i - 1][0]))
                bad++;
            if (punct_is_logical(levels[i][j]) != (levels[i][j] == P_ANDAND || levels[i][j] == P_OROR))
                bad++;
        }
    if (punct_prec(P_COMMA) != PR_COMMA || PR_COMMA != 0 || punct_prec(P_NONE) != 0 ||
        punct_prec(-1) != 0 || punct_prec(P_COUNT) != 0)
        bad++;
    for (l = 0; l < P_COUNT; l++) {
        if (punct_is_assign(l)) {
            const char *s = punct_spelling[l], *b = punct_spelling[punct_assign_base(l)];
            size_t n = strlen(s);
            if (l == P_ASSIGN) {
                if (punct_assign_base(l) != l)
                    bad++;
            } else if (punct_assign_base(l) == l || !punct_prec(punct_assign_base(l)) ||
                       n != strlen(b) + 1 || strncmp(s, b, n - 1) || s[n - 1] != '=') {
                fprintf(stderr, "punct selftest: %s\n", s);
                bad++;
            }
        } else if (punct_assign_base(l) != l) {
            bad++;
        }
    }
    if (bad)
        fprintf(stderr, "punct selftest: %d failures\n", bad);
    return bad != 0;
}
```

(`lex.c` includes `<string.h>`; add `<stdio.h>` and `<stdint.h>` if the build says so.)

- [ ] **Step 5: Wire the dispatch and the test run**

`src/main.c`, after the `diag` dispatch (the line `return diag_selftest();`):

```c
    if (argc == 3 && !strcmp(argv[1], "selftest") && !strcmp(argv[2], "punct"))
        return punct_selftest();
```

`tests/run.sh`, after the `selftest diag` line:

```sh
if "$CEREAL" selftest punct </dev/null; then ok; else bad "selftest punct"; fi
```

- [ ] **Step 6: Build and run**

Run: `MSYS_NO_PATHCONV=1 wsl -e bash -c 'cd ~/cereal-s1 && sh .sync.sh >/dev/null 2>&1; make -j4 2>&1 | grep -E "error|warning"; ./cereal selftest punct </dev/null; echo rc=$?'`
Expected: no compiler output, `rc=0`. If a check fails the message names it; fix the table, not the test, unless the test contradicts C (then the test is wrong).

- [ ] **Step 7: Prove the selftest can fail**

Temporarily change `[P_LE] = PI(PR_REL, PF_CMP, P_GE, P_GT, 0)` to `PI(PR_REL, PF_CMP, P_GE, P_LT, 0)` in `lex.c`, rebuild, run `./cereal selftest punct </dev/null; echo rc=$?`. Expected: `rc=1`. Revert the change and rebuild; expected `rc=0` again.

- [ ] **Step 8: Globals and commit**

Run: `MSYS_NO_PATHCONV=1 wsl -e bash -c 'cd ~/cereal-s1 && sh bench/tools/globals.sh 2>&1 | tail -2'`. Expected: `all allowed` (the table is const).

```bash
cd "/c/Users/Winston Li/Documents/GitHub/cereal" && git add src tests && git commit -m "AR2 T1: punct_info table, rel_holds, selftest punct

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

No full gate: nothing outside the selftest uses the table yet.

---

### Task 2: Precedence

**Files:**
- Modify: `src/c/parse.c` (`binary_prec` 2349-2366, `is_assign_op` 2368-2381, `binary()` 2810-2830, use at 2849)
- Modify: `src/analysis/pcexpr.c` (`prec` 184-201, `px_binary` 203-222)
- Modify: `src/c/cprint.c` (`bin_prec` 21-37 and its uses)
- Modify: `src/analysis/hygiene.c:93-105` (`is_binary_op`)

**Interfaces:** consumes `punct_prec`, `tok_prec`, `punct_is_assign`, `PR_*` (T1).

- [ ] **Step 1: `parse.c`**

Delete `binary_prec` and `is_assign_op`. In `binary()` replace `int prec = binary_prec(&t);` with `int prec = tok_prec(&t.t);`; replace `binary(p, 4);` (line 2829) by `binary(p, PR_LOR);`; at line 2849 replace `is_assign_op(&t)` by `(t.t.kind == TK_PUNCT && punct_is_assign(t.t.punct))`.

- [ ] **Step 2: `pcexpr.c`**

Delete `prec`. In `px_binary` replace `int pr = prec(p->t);` by `int pr = tok_prec(p->t);` and `if (pr < min || pr < 0)` by `if (pr < min)` (min is at least `PR_LOR`, so 0 returns). Replace `px_binary(p, 1)` (line 222) by `px_binary(p, PR_LOR)`.

- [ ] **Step 3: `cprint.c`**

Delete `bin_prec`; replace every `bin_prec(` with `punct_prec(`: `sed -i 's/\bbin_prec(/punct_prec(/g' src/c/cprint.c`. Check `grep -n "punct_prec" src/c/cprint.c` shows the uses at the old lines (240, 255, 262, 270, 336, 382).

- [ ] **Step 4: `hygiene.c`**

Replace the body of `is_binary_op` (the `switch`) by:

```c
    return punct_prec(p) != 0 || p == P_QUESTION;
```

(the old list is exactly the binary operators plus `?`).

- [ ] **Step 5: Build, quick loop, then the full gate**

Run the quick loop of the gate recipe. Expected: no compiler output; `N passed, 0 failed`.
Run the full gate with `-Name ar2-t2`. Expected: as in the recipe.

- [ ] **Step 6: Commit**

```bash
git add src && git commit -m "AR2 T2: one precedence table (parse, pcexpr, cprint, hygiene)

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Property helpers and case lists

**Files:**
- Modify: `src/c/cwarn_expr.c` (`lg_cmp`/`lg_swap`/`lg_inv` 33-57, `is_cmp_op` 417, `swap_cmp` 435, uses; case lists at 144, 278, 460, 543, 1181)
- Modify: `src/c/cstmt_warn.c` (`dup_mirror`/`dup_negate` 946-967, uses 1034-1091, case lists 987, 1008, 1125)
- Modify: `src/analysis/pcexpr.c` (`is_cmp`, `swap_cmp` 684-698, uses 961-968)
- Modify: `src/c/tast_vec.c` (`is_relop` 203-206, uses 220, 233)
- Modify: `src/c/cexpr.c` (`assign_binop` 6810-6826, use 6885, `is_cmp_op` use 6581), `src/c/check_int.h:872`, `src/c/cexpr_int.h:344`, `src/c/cconv.c` (863 redeclaration, 873, 271, 498, 527)
- Modify: `src/c/cstmt.c:515`, `src/c/cseqpt.c:560`

**Interfaces:** consumes `punct_is_cmp`, `punct_is_bool`, `punct_swap`, `punct_negate`, `punct_assign_base` (T1). Produces: none.

- [ ] **Step 1: Pure renames, with sed**

```bash
cd src && sed -i 's/\blg_cmp(/punct_is_cmp(/g; s/\blg_swap(/punct_swap(/g; s/\blg_inv(/punct_negate(/g; s/\bis_cmp_op(/punct_is_cmp(/g; s/\bswap_cmp(/punct_swap(/g' c/cwarn_expr.c c/cexpr.c c/cconv.c analysis/pcexpr.c
sed -i 's/\bdup_mirror(/punct_swap(/g; s/\bdup_negate(/punct_negate(/g' c/cstmt_warn.c
sed -i 's/\bis_relop(/punct_is_cmp(/g' c/tast_vec.c
sed -i 's/\bassign_binop(/punct_assign_base(/g' c/cexpr.c c/cconv.c
sed -i 's/\bis_cmp(/punct_is_cmp(/g' analysis/pcexpr.c
sed -i 's/\r$//' c/cwarn_expr.c c/cexpr.c c/cconv.c analysis/pcexpr.c c/cstmt_warn.c c/tast_vec.c
```

(Do this before deleting the definitions so the definitions get renamed too; the next step deletes them.)

- [ ] **Step 2: Delete the now-renamed definitions and declarations**

Delete, each with its comment: in `cwarn_expr.c` the functions `punct_is_cmp` (was `lg_cmp`, near line 33), `punct_swap` (`lg_swap`), `punct_negate` (`lg_inv`), and the second `punct_is_cmp` (`is_cmp_op`, near 417), and `punct_swap` (`swap_cmp`, near 435); keep `cmp_str`. In `cstmt_warn.c` delete `punct_swap`/`punct_negate` definitions (were `dup_mirror`/`dup_negate`). In `pcexpr.c` delete `punct_is_cmp` and `punct_swap` (were `is_cmp`, `swap_cmp`). In `tast_vec.c` delete `punct_is_cmp` (was `is_relop`). In `cexpr.c` delete `punct_assign_base` (was `assign_binop`, 6810-6826). Delete `bool is_cmp_op(int op);` from `cexpr_int.h:344`, the redeclaration at `cconv.c:863`, and `int assign_binop(int op); ...` from `check_int.h:872`. Check: `grep -rn "static.*punct_\(is_cmp\|swap\|negate\|assign_base\)\|^\(bool\|int\) punct_" src` finds nothing but `token.h`'s inlines.

- [ ] **Step 3: Case lists become predicates**

Apply these rewrites (each site is read first; the list must equal the predicate):

- `cwarn_expr.c` 144-146: `(punct_is_cmp(npunct(c, n)) || npunct(c, n) == P_ANDAND || npunct(c, n) == P_OROR)` -> `punct_is_bool(npunct(c, n))`.
- `cwarn_expr.c` 278-279: `(punct_is_cmp(pa) || pa == P_ANDAND || pa == P_OROR)` -> `punct_is_bool(pa)`.
- `cwarn_expr.c` 1181: `punct_is_cmp(rop) || rop == P_ANDAND || rop == P_OROR ||` -> `punct_is_bool(rop) ||`.
- `cwarn_expr.c` `truth_expr` (about 458-467): the `switch (npunct(c, n)) {... return true; default: return false;}` becomes `return punct_is_bool(npunct(c, n));`.
- `cwarn_expr.c` about 543-546 (`nonneg`, inside its `case N_BINARY:`): the inner `switch (npunct(c, n))` keeps its other cases; delete the two case lines `case P_EQEQ: ... case P_GE:` / `case P_ANDAND: case P_OROR:` with their `return true;`, and insert `if (punct_is_bool(npunct(c, n))) return true;` as the first statement after `case N_BINARY:`, before that inner `switch`.
- `cconv.c` 527-528: `return p == P_EQEQ || ... || p == P_ANDAND || p == P_OROR;` -> `return punct_is_bool(p);`.
- `cseqpt.c` 560-563 (same shape as `truth_expr`): `return punct_is_bool(...)`.
- `cstmt.c` 515-517: the case list `P_EQEQ ... P_GE, P_ANDAND, P_OROR, P_COMMA` -> `if (punct_is_bool(npunct(c, e)) || npunct(c, e) == P_COMMA) return false;` before the `switch`, then drop those case lines.
- `cstmt_warn.c` 987-990 and 1008-1011: `switch (dup_punct(...)) { case <six comparisons>: ...; default: break; }` -> `if (punct_is_cmp(dup_punct(c, in))) { e = in; goto cmp; }` (resp. `if (punct_is_cmp(dup_punct(c, e))) goto cmp;`).
- `cstmt_warn.c` 1121-1131 (`dupcond_loc`): the body becomes
  ```c
      if (tg(c, e) == N_BINARY && !punct_is_bool(dup_punct(c, e)) &&
          dup_punct(c, e) != P_COMMA && !dup_const(c, e))
          return ctok_loc(c, first_tok(c, e));
      return cnode_loc(c, e);
  ```

Not changed (not a property): `cexpr.c:6590` and `tast.c:1089` (dispatch switches), `cseqpt.c:399` (mixed list), `cwarn_expr.c` interval/range switches (about 184, 853), `cgx86.c`, `tast_x86.c`, `eval.c:1877,1891`, `pcexpr.c:676`.

- [ ] **Step 4: `cstmt_warn.c` negate default**

`dup_negate` returned `P_LE` for any non-listed operator; `punct_negate` returns 0 for a non-comparison. Every caller passes a comparison (`d->op` is `P_EQEQ P_NE P_LT P_LE P_GT P_GE`, see the field comment at line 940), so nothing changes; check with `grep -n "punct_negate" src/c/cstmt_warn.c src/c/cwarn_expr.c` and read each call.

- [ ] **Step 5: Build, grep, quick loop, full gate**

Run: `cd src && grep -rn "lg_cmp\|lg_swap\|lg_inv\|is_cmp_op\|swap_cmp\|dup_mirror\|dup_negate\|is_relop\|assign_binop" . | head`
Expected: no output.
Quick loop, then the full gate with `-Name ar2-t3`; expected as in the recipe (the warning goldens are the net for this task).

- [ ] **Step 6: Commit**

```bash
git add src && git commit -m "AR2 T2b: property helpers from punct_info (cmp, swap, negate, assign base, bool)

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 4: `rel_holds`

**Files:**
- Create: `tests/eval/rel_ops.c`
- Modify: `src/c/eval.c` (545-556 floats, 708-720 ints), `src/ppexpr.c:611-625`, `src/analysis/pcexpr.c` (about 727-740), `src/c/cwarn_expr.c` (about 896-906, 1150-1155, 2425-2495)

**Interfaces:** consumes `rel_holds`, `rel_cmp_s`, `rel_cmp_u`, `rel_cmp_f` (T1).

- [ ] **Step 1: The test first (`tests/eval/rel_ops.c`)**

```c
/* the six relations on signed, unsigned, floating and pointer operands, run and folded;
 * eval.sh compares the evaluator with gcc */
#include <stdio.h>

#define SIX(a, b) (((a) < (b)) + 2 * ((a) > (b)) + 4 * ((a) <= (b)) + 8 * ((a) >= (b)) + \
                   16 * ((a) == (b)) + 32 * ((a) != (b)))

static int si(long long a, long long b) { return SIX(a, b); }
static int ui(unsigned long long a, unsigned long long b) { return SIX(a, b); }
static int fl(double a, double b) { return SIX(a, b); }
static int sh(int a, unsigned b) { return SIX(a, b); }      /* -1 against 1 becomes unsigned */

static int arr[4];

int main(void)
{
    volatile double zero = 0.0;
    double nan = zero / zero;
    long long s[] = {-2, -1, 0, 1, 2, 0x7fffffffffffffffLL, -0x7fffffffffffffffLL - 1};
    unsigned long long u[] = {0, 1, 2, ~0ULL, 1ULL << 63};
    double d[] = {-1.5, 0.0, 1.5, 1e300, 0.0 / 1.0};
    int i, j, r = 0;
    for (i = 0; i < 7; i++)
        for (j = 0; j < 7; j++)
            r = r * 31 + si(s[i], s[j]);
    for (i = 0; i < 5; i++)
        for (j = 0; j < 5; j++)
            r = r * 31 + ui(u[i], u[j]) + fl(d[i], d[j]);
    for (i = 0; i < 5; i++)
        r = r * 31 + fl(nan, d[i]) + 64 * fl(d[i], nan) + 4096 * fl(nan, nan);
    printf("%d %d %d\n", sh(-1, 1), sh(1, 1), sh(0, 1));
    printf("%d %d %d %d\n", SIX(1, 2), SIX(2u, 1u), SIX(-1, 1u), SIX(1.5, 1.5));
    printf("%d %d %d\n", SIX(&arr[0], &arr[2]), SIX(&arr[3], &arr[3]), SIX(&arr[3], &arr[1]));
    printf("%d\n", r);
    return r & 127;
}
```

- [ ] **Step 2: Run it against the current build**

Run: `MSYS_NO_PATHCONV=1 wsl -e bash -c 'cd ~/cereal-s1 && sh .sync.sh >/dev/null 2>&1; sh tests/eval.sh ./cereal 2>&1 | tail -3'`
Expected: the summary line shows 0 failed (the new case passes before any change: it is a characterization against gcc). If `rel_ops` fails, fix the test file (the program must be valid C that gcc runs) before touching `src`.

- [ ] **Step 3: `eval.c`**

Floats (545-556): replace the switch by

```c
    if (!punct_is_cmp(p)) {
        UNSUP(v, "comparison %d", p);
        return 0;
    }
    return rel_holds(p, rel_cmp_f(x, y));
```

Ints (708-720): replace the switch by

```c
    if (!punct_is_cmp(p)) {
        UNSUP(v, "comparison %d", p);
        return 0;
    }
    return rel_holds(p, sg ? rel_cmp_s((int64_t)a, (int64_t)b) : rel_cmp_u(a, b));
```

(keep whatever `UNSUP` and `return` the surrounding function already has after the old `switch`; read lines 535-560 and 700-725 first.)

- [ ] **Step 4: `ppexpr.c` 611-625**

Replace the `switch (op->punct) {...}` with

```c
        r = rel_holds(op->punct, a.uns ? rel_cmp_u(a.v, b.v) : rel_cmp_s(sv(a), sv(b)));
```

- [ ] **Step 5: `pcexpr.c` about 727-740**

Replace the six cases `P_LT P_GT P_LE P_GE P_EQEQ P_NE` of the `fold` switch by one:

```c
    case P_LT: case P_GT: case P_LE: case P_GE: case P_EQEQ: case P_NE:
        r = rel_holds(op, uns ? rel_cmp_u(ux, uy) : rel_cmp_s(x->n, y->n));
        ru = false;
        break;
```

(equality of `ux`/`uy` and of `x->n`/`y->n` agree, as both are the same bits; read the lines above to confirm `ux`, `uy`, `uns` and `op` are the names in scope.)

- [ ] **Step 6: `cwarn_expr.c`**

- About 896-906 (`bool-compare`): replace the `switch (op) {... default: r[x] = cmp >= 0; }` by `r[x] = rel_holds(op, cmp);`.
- About 1152-1155 (`tautological-compare`): `(op == P_EQEQ || op == P_LE || op == P_GE) ? "true" : "false"` -> `rel_holds(op, 0) ? "true" : "false"`.
- About 2430-2437: `switch (op) { case P_EQEQ: r = equal; ... default: r = cmp >= 0; }` -> `r = rel_holds(op, cmp);` (`equal` is `cmp == 0` here: `cba == cbb`, so `equal == (va == vb)`).
- About 2444-2445: `r = op == P_LT ? cmp < 0 : op == P_GT ? cmp > 0 : op == P_LE ? cmp <= 0 : cmp >= 0;` -> `r = rel_holds(op, cmp);`.
- About 2472-2478: the integer `switch` -> `r = rel_holds(op, cmp);`.
- About 2486-2492: the floating `switch` -> `r = rel_holds(op, rel_cmp_f(fa, fb));`.

Not changed: the interval switches around 184 and 853 (they compute value ranges, not truth values).

- [ ] **Step 7: Verify**

Run: `cd src && grep -n "case P_LE" c/eval.c ppexpr.c analysis/pcexpr.c c/cwarn_expr.c`
Expected: only `eval.c` 1877 and 1891 (the `SX_` lowering), `pcexpr.c` 676 (`lin_cmp`) and the two `cwarn_expr.c` interval switches (about 184 and 853).
Quick loop (it includes `tests/eval.sh` and the new case), then the full gate with `-Name ar2-t4`. Expected as in the recipe; record the callgrind number.

- [ ] **Step 8: Commit**

```bash
git add src tests && git commit -m "AR2 T3: rel_holds for the nine relation switches; tests/eval/rel_ops.c

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Docs and close

**Files:**
- Modify: `docs/CLEANUP_PLAN.md` (AR2 and FE6 lines), `docs/AR2_DESIGN.md` (status, amended text), `docs/AR2_PLAN.md` (status)
- Check: `docs/ARCHITECTURE.md`, `docs/PARSER.md`, `docs/TYPES.md`, `docs/HISTORY.md`, `PARKED.md`, `ROADMAP.md`

- [ ] **Step 1: Find stale wording**

Run: `grep -rn "swap_cmp\|is_cmp_op\|assign_binop\|bin_prec\|binary_prec\|dup_mirror\|dup_negate\|lg_swap\|lg_inv\|lg_cmp\|is_relop\|is_assign_op" docs *.md | grep -v "^docs/CLEANUP_PLAN.md\|^docs/AR2_"`
For each hit: dated history stays (cite with `git show <commit>:<path>` if it explains a past state); text describing live code is edited to name `punct_info`/`punct_*`/`rel_holds`.

- [ ] **Step 2: Amend the spec**

In `docs/AR2_DESIGN.md`: set `status: done`; in the table text say `punct_assign_base` returns the operator itself when it is not a compound assignment and `base` 0 means that; drop the `cmp_str` bullet from Non-goals' replace-it suggestion (it stays: it is passed non-comparison operators); name the helpers actually deleted (`lg_cmp`, `lg_swap`, `lg_inv`, `is_cmp_op`, two `swap_cmp`, `dup_mirror`, `dup_negate`, pcexpr `is_cmp`, `is_relop`, `assign_binop`, `binary_prec`, `is_assign_op`, pcexpr `prec`, `bin_prec`) and the nine value switches.

- [ ] **Step 3: Record the outcome in the cleanup plan**

Append to the AR2 item in `docs/CLEANUP_PLAN.md`: `**Done 2026-MM-DD** (commits): ...` with the commits of T1-T4, the helpers and sites removed (counts from `git diff --stat <T1 parent>..HEAD`), the four gate run ids, the callgrind number against 3 890 870 693, and `Not done:` whatever remained (the lowering switches and AR3). Change the FE6 line to `Done by AR2`.

- [ ] **Step 4: Plan status and commit**

Set `status: done` in `docs/AR2_PLAN.md`.

```bash
git add docs && git commit -m "AR2: close the plan; docs name punct_info and rel_holds

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

## Self-review

- **Spec coverage:** table and accessors (T1), precedence incl. `PR_*` move and start values (T2), swap/negate/cmp/assign_base/bool and extern removal (T3), `rel_holds` with `REL_UNORDERED` (T4), `selftest punct` (T1), per-stage gate and callgrind record (T2-T4), docs (T5). The spec's `punct_is_logical` is used by `selftest` and `punct_is_bool` only; `uint8_t` guard is the `_Static_assert` in T1.
- **Placeholders:** none; the two sites whose exact text is not reproduced (the `nonneg` list near `cwarn_expr.c:543` and the `eval.c` `UNSUP` tails) say what to read and how to rewrite.
- **Type consistency:** `punct_*` take `int`; `rel_holds(int, int)`; `rel_cmp_s(int64_t, int64_t)`, `rel_cmp_u(uint64_t, uint64_t)`, `rel_cmp_f(long double, long double)`; `tok_prec(const Tok *)`; `PunctInfo` has five `uint8_t` fields in the order `prec, flags, swap, negate, base`, which the table initialisers follow.
- **Review focus:** items 1-5 above each have a test in T1 or T4.
