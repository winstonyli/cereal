---
status: active
---

Stress test (2026-10-31, by hand against the source): round 1 checked the three precedence orders, the swap/negate identities, `cmp_str` against `punct_spelling`, the `is_relop` copy in `tast_vec.c` and the Pratt start values; it added the `PR_LOR` start values and the `is_relop` entry. Remaining: the full `case P_LE:` site list is taken at plan time.

# AR2: one operator-property table and `rel_holds`

Source: `docs/CLEANUP_PLAN.md` item AR2 (subsumes FE6). Chosen scope 2026-10-31: properties plus `rel_holds` (option i1).

## Goal

The properties of a punctuator (is it a comparison, its mirror and negation, the binary operator of a compound assignment, its binary precedence) are stated once, in a table keyed by `Punct`, and the relation of two compared values is decided in one function. Today 35 `case P_LE:` sites in 13 files and about 12 helpers restate them, each with the cases it happens to need.

## Non-goals

- Arithmetic of the integer operators (`cexpr.c`, `eval.c` `P_PLUS` ...): AR3.
- Lowering and symbolic switches that map an operator to something else: `cgx86.c:605` (condition codes), `eval.c:1877` and `:1891` (`SX_*` ops), `pcexpr.c:676` (BDD of a linear constraint). They stay switches; they may use `punct_swap`/`punct_negate` where they have a private copy.
- The two parsers stay two (C `parse.c`, presence-condition `pcexpr.c`); only their precedence numbers come from the table.
- `cmp_str` (`cwarn_expr.c:424`) is spelling, and `punct_spelling` already holds it: replace it by `punct_spelling[op]` if the strings are the same.

## Design

### The table

`token.h` owns `Punct` and `PUNCT_LIST`; `lex.c` owns `punct_spelling`. Beside the spellings, `lex.c` gets

```c
typedef struct {
    uint8_t prec;       /* binary precedence (PR_* scale), 0: not a binary operator */
    uint8_t flags;      /* PF_CMP, PF_ASSIGN, PF_LOGICAL */
    uint8_t swap;       /* Punct: a OP b == b swap(OP) a; the operator itself if none */
    uint8_t negate;     /* Punct: !(a OP b) == a negate(OP) b; P_NONE if none */
    uint8_t base;       /* Punct: the binary operator of a compound assignment; P_NONE if none */
} PunctInfo;
extern const PunctInfo punct_info[P_COUNT];
```

(`uint8_t` holds `Punct` only while `P_COUNT` < 256; a `_Static_assert` says so.) The initialiser uses designated entries (`[P_LE] = {PR_REL, PF_CMP, P_GE, P_GT, P_NONE}`); an operator with no property has no entry. `token.h` declares static inlines over it: `punct_prec`, `punct_is_cmp`, `punct_is_assign`, `punct_is_logical`, `punct_swap`, `punct_negate`, `punct_assign_base`. They tolerate any `int` below `P_COUNT` and return the neutral value for others (the callers hold `int op`).

The precedence scale moves from `cexpr_int.h` to `token.h`: `PR_COMMA ... PR_PRIMARY` (the enum names stay). `PR_COMMA` is 0, which is also "not a binary operator" for the two Pratt parsers; the comma is never handled by them (the C parser's comma is a separate production) and `cprint.c` only asks about nodes that are binary. `parse.c`'s 4..13 and `pcexpr.c`'s 1..10 only compare against each other and against `min`: they become `punct_prec` against `PR_LOR`-based minimums, and the operators between `PR_LOR` and `PR_MUL` have the same order in all three tables (checked 2026-10-31 against `parse.c:2349`, `pcexpr.c:184`, `cprint.c:21`). The two start values (`parse.c:2829` `binary(p, 4)`, `pcexpr.c:222` `px_binary(p, 1)`) become `PR_LOR`. `cprint.c` also treats an unlisted binary operator as `PR_MUL`; the table lists `* / %` explicitly, so the default goes.

### `rel_holds`

```c
/* does `a OP b` hold, given cmp = (a > b) - (a < b), or REL_UNORDERED */
enum { REL_UNORDERED = 2 };
static inline bool rel_holds(int op, int cmp);
```

`P_EQEQ` holds for 0, `P_NE` for everything but 0 (and for `REL_UNORDERED`), `P_LT` for -1, `P_GT` for 1, `P_LE` for -1 and 0, `P_GE` for 0 and 1; every ordered relation but `!=` is false when unordered. The caller makes `cmp`: signed or unsigned compare of its operands, or the floating compare that yields `REL_UNORDERED` for a NaN. This replaces the four value switches `eval.c:551` (doubles), `eval.c:716` (ints), `pcexpr.c:734` and `ppexpr.c:620` (the latter two keep their own signed/unsigned choice and pass the three-way result).

### Sites, by kind

1. Properties (helpers deleted): `is_cmp_op` (`cwarn_expr.c:417`, extern in `cexpr_int.h:344`), both `swap_cmp` (`cwarn_expr.c:435`, `pcexpr.c:690`), `dup_mirror`, `dup_negate` (`cstmt_warn.c:947`, `:960`), `assign_binop` (`cexpr.c:6810`, extern in `check_int.h:872`), `parse.c` `binary_prec` and `is_assign_op`, `pcexpr.c` `prec`, `cprint.c` `bin_prec`, `tast_vec.c` `is_relop`.
2. Case lists "is a comparison" (become `punct_is_cmp(op)`): `cwarn_expr.c` (about 10 of its 12), `cstmt.c:515`, `cstmt_warn.c:987,1008,1125`, `cexpr.c:6590`, `tast.c:1089`, `cseqpt.c:560`, and `truth_expr` at `cwarn_expr.c:450` (cmp or logical). Sites whose list is not exactly "comparison" (`cseqpt.c:399`, `hygiene.c:97` mix other operators; `cgx86.c:605`, `eval.c:1877`) are read case by case and left alone if they are not.
3. Value switches (become `rel_holds`): the four above.

The full site list is fixed in the plan from a fresh grep of `case P_LE:`; line numbers drift.

### Interface changes

`is_cmp_op` and `assign_binop` disappear, with their declarations. `PR_*` moves header. Nothing else crosses a file boundary: the other helpers are file-local. Every caller is fixed once; no wrappers, no compatibility names.

## Checks

- Hidden `cereal selftest punct`, run from `tests/run.sh` like `bdd`, `pcq` and `diag`:
  - `punct_swap` and `punct_negate` are involutions on comparisons; `negate(swap(op)) == swap(negate(op))`;
  - `rel_holds(op, cmp)` equals C's own `a OP b` over a grid of ints and doubles, NaN included;
  - `punct_assign_base(op)` is a binary operator with `punct_prec` > 0 for every assignment but `=`;
  - the binary operators are in non-decreasing precedence in the order `||`, `&&`, `|`, `^`, `&`, `== !=`, `< > <= >=`, `<< >>`, `+ -`, `* / %`.
- Every stage ends with the full gate (`bench/tools/gate.sh`): the goldens cover the warnings, `-E` and the evaluator; verify shows diagnostics unchanged; the callgrind count is recorded (it should not rise by more than layout noise, `docs/CLEANUP_PLAN.md` FE11).
- `globals.sh` stays at the same count (the table is `const`).

## Stages

Each is its own commit with a green gate.

1. **Table and precedence.** `PunctInfo`, the accessors, the `PR_*` move, `selftest punct`; `parse.c`, `pcexpr.c`, `cprint.c` use `punct_prec`; `binary_prec`, `is_assign_op`, `prec`, `bin_prec` deleted.
2. **Properties and case lists.** `punct_is_cmp`, `punct_swap`, `punct_negate`, `punct_assign_base` at the sites of kinds 1 and 2; `is_cmp_op`, `assign_binop` and the file-local copies deleted; docs updated.
3. **`rel_holds`.** The four value switches; the NaN case of `eval.c:551` is covered by a `tests/` case (a program comparing a NaN with `<`, `>=`, `==`, `!=`) if none exists.

## Risks

- The float switch: `a <= b` is false for NaN, so `rel_holds(P_LE, 2)` must be false and `rel_holds(P_NE, 2)` true. The selftest covers it; stage 3 also needs an evaluator golden.
- `uint8_t` in the table: guarded by the `_Static_assert` on `P_COUNT`.
- A case list that looked like "comparison" but included one more operator would change behaviour when replaced; each site is read before it is replaced, and the stage-2 gate (warnings goldens, 2000+ cases, verify against gcc) is the net.

## Done when

No `case P_LE:` remains outside the lowering, symbolic and arithmetic switches named under Non-goals; `grep -n "swap_cmp\|dup_mirror\|dup_negate\|is_cmp_op\|assign_binop\|binary_prec\|bin_prec" src` finds nothing; the table, `rel_holds` and `selftest punct` are the one home for these facts.
