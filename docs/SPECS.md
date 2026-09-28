# Specs and UB elimination: from-scratch design brainstorm

Status: **brainstorm**. Everything here is open; the forks are listed at the end.

## 1. What the goals force

| Goal | Consequence for the logic and syntax |
|---|---|
| UB elimination is the product; functional correctness is a bonus | The unit of work is a **UB obligation**. Every UB site in C99 Annex J.2 that is decided at run time generates one. Specs exist to discharge obligations, not the other way round. |
| `checked` mode = trap only where not proven | **Every spec must be executable.** An unproven obligation or contract compiles to a runtime check, so the spec language is essentially C expressions. Non-executable constructs are explicitly ghost-only. |
| Compile time is the priority | Most obligations must be discharged **without a solver**: syntactically, by abstract interpretation, or by a permission checker. The solver is the last tier, never the first. |
| Parallel, cached | **The function is the unit of verification.** Contracts are the only interface between functions, so a callee's body can change without invalidating its callers' proofs. |
| Built-in prover, cache keys | **Determinism.** Budgets are measured in fuel (solver steps), never wall-clock time, and the same input always gives the same verdict. |
| Huge generated files | Cost stays linear in code size. Identical-shaped functions **share proofs** through structural (alpha-normalized) hashing, and generators emit specs with macros. |
| Specs are preprocessor directives | Specs are pp-tokens: macros can generate them, `#if` can condition them, and the skeleton pass and the index see them. |

## 2. Core model: four tiers of discharge

```
obligation ─► T0 syntactic ─► T1 abstract interp. ─► T2 permission check ─► T3 solver ─► strict: error
             (constant idx,   (intervals, congru-    (ownership/aliasing,   (DPLL(T):   checked: trap
              literal shift)   ences, zones, null,    linear-type-like,      LIA, EUF,   off: nothing
                               init-state, pointer    no solver)             slices,
                               provenance+offset)                            bitvectors)
```

- **T0/T1 need no annotations.** Abstract interpretation also *infers* function summaries bottom-up over the call graph. The LSP can show inferred contracts and offer to insert them into the source.
- **T2** handles aliasing and lifetime (use-after-free, double free, write–write races on the same object within one expression, `restrict`). It uses declared *permission modes* (§4), not logic formulas, and runs as a linear-time dataflow pass.
- **T3** is reached only by obligations that still carry arithmetic or data-dependent facts. Fuel is per obligation. A counterexample from the solver's model becomes a concrete input shown in the editor, and doubles as a test case.

Per-obligation status: `proved(T0..T3)` / `runtime-check` / `failed(counterexample)` / `assumed`. That status is what the LSP renders, as inlay hints or gutter marks.

## 3. Spec expression language

**C99 expressions, plus as few extensions as possible, all spelled with existing pp-tokens.** The C parser is reused, and the checked-mode code generator compiles specs like ordinary code.

| Need | Spelling | Why |
|---|---|---|
| Return value | `return` | A keyword that can never be a C expression, so it can't be confused with a variable or macro, and needs no `\result`. |
| Pre-state | `$old(e)` | The `$`-namespace holds all spec builtins. Strict C99 can't use `$` identifiers, so user macros can't collide with them. |
| Ranges | `p[lo:hi]` (half-open) | A slice. The `:` token keeps `lo` and `hi` separate tokens, which `0..n` would not (it lexes as one pp-number), so macro expansion still works. |
| Elementwise facts | `a[0:n] >= 0`, `d[0:n] == s[0:n]` | **Ranges replace quantifiers** in the common case. Facts over ranges split and merge cheaply, with no quantifier instantiation. |
| Explicit quantifier | `$all(i, lo, hi, e)`, `$any(i, lo, hi, e)` | Bounded, so it is executable as a loop. Unbounded forms (`$all(i, e)`) are ghost-only. |
| Implication | `a ==> b` | `==` `>` side by side is never valid C. |
| Memory facts | `$valid(p)`, `$len(p)`, `$init(p[a:b])`, `$fresh(p)`, `$disjoint(r1, r2)` | These are about pointer provenance and object bounds (§5). |

- **Integer semantics.** Spec arithmetic is **mathematical**: `a + b <= INT_MAX` is a meaningful precondition, not itself an overflow. Checked mode evaluates it with overflow-detecting widening.
- **Well-definedness.** A spec expression must itself be UB-free: `p != 0 && p[0] > 0` passes, `p[0] > 0` alone fails. This is checked with the same machinery.
- **Macro expansion.** Specs *are* macro-expanded, so they can use `BUF_SIZE` and friends. `return` and `$…` builtins are recognized before expansion, the same way `defined` is in `#if`.

## 4. Memory: permission modes (the main fork)

C's UB is dominated by memory, and solving memory with logic formulas is the expensive part of every verifier. The proposal is a **permission layer checked like a type system** (T2). Formulas are left for arithmetic.

```c
#param dst  out[n]     /* write-only until written; initialized on return */
#param src  in[n]      /* read-only for the call; may not alias any out/inout */
#param ctx  inout      /* one object, read/write */
#param p    own        /* ownership transferred to callee (e.g. free) */
#result     own[len]   /* caller receives a fresh allocation of len elements */
#result     borrow(ctx)/* returned pointer lives no longer than ctx */
```

- Modes: `in`, `out`, `inout`, `own`, `borrow(x)`, `raw`. `raw` is an escape hatch: plain C semantics, with every access obligation left to T1/T3 or a runtime check.
- `[n]` gives the extent as an expression. `n` may name other parameters or call `$len`.
- Non-aliasing follows from the modes (`out`/`inout` are exclusive) instead of being written as `$disjoint` everywhere.
- Unannotated pointer parameters default to `raw`, so the scheme can be adopted gradually.
- **Alternative M1:** no modes; write `$valid`/`$disjoint`/`#modifies` formulas and let T3 handle aliasing (the Frama-C/ACSL style). More uniform, far slower.
- **Alternative M3:** user-defined predicates over permissions, for recursive structures such as lists and trees. Proposed as a later extension on top of the modes, not instead of them.

## 5. The object model the prover assumes

- An allocation has a base, a size, a lifetime and an effective type.
- A pointer is `(provenance, offset)`, `null`, or indeterminate.
- The obligations follow directly:
  - dereferencing needs a live allocation, the offset within bounds, correct alignment, and a compatible effective type;
  - pointer arithmetic must stay within `[0, size]`;
  - relational comparison needs the same provenance.
- **Checked mode runtime** needs bounds only for pointers whose accesses were not proved. Candidate: *low-fat pointers*, where allocation size classes make bounds computable from the pointer value. That means no ABI change and no shadow memory. The metadata is paid for only where proof failed.

## 6. Directive syntax

### 6.1 Line directives (attach to the next declaration or statement)

```c
#requires n <= $len(dst) && n <= $len(src)
#param    dst out[n]
#param    src in[n]
#ensures  dst[0:n] == src[0:n] && return == dst
char *copy(char *dst, const char *src, size_t n);

#invariant i <= n && dst[0:i] == src[0:i]
for (size_t i = 0; i < n; i++) dst[i] = src[i];

#assert  $init(buf[0:len])
#assume  x != 0            /* trusted; listed by `cereal audit` */
#ghost   total += n;       /* erased by codegen */
```

Portable spelling: `#pragma cereal requires …` (unknown pragmas are ignored, C99 6.10.6). Inside macros: `_Pragma("cereal requires …")`. A `cereal portable` rewrite converts between the forms.

### 6.2 Block form for definitions, lemmas and ghost code: `#if $spec`

```c
#if $spec
$pure _Bool sorted(const int *a, size_t n) {
    return $all(i, 1, n, a[i - 1] <= a[i]);
}
$lemma void sorted_prefix(const int *a, size_t n, size_t k)
#requires sorted(a, n) && k <= n
#ensures  sorted(a, k)
{ }
#endif
```

**This block form is fully portable with no new syntax.** For GCC and Clang, `$spec` is an undefined identifier, so the condition is `0` and the group is skipped. For code generation cereal also treats it as `0`. The spec layer reads the group, because the skeleton pass sees inactive regions anyway. Multi-line specs need no backslashes, and `#ifdef` inside spec blocks works naturally.

### 6.3 Policy directives (scoped, stackable)

```c
#verify push strict(bounds, null, lifetime) checked(overflow) off(alias)
...
#verify pop
```

The policy is set per UB class, not per whole function, so a hot inner loop can be `strict(bounds)` (no checks allowed) while still checking overflow.

### 6.4 Macro contracts

```c
#requires $pure(a) && $pure(b)          /* no side effects in arguments */
#requires $expr                         /* must be used in expression position */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
```

These are checked at every expansion site. The hygiene lints become *declarable* properties, and violations are reported at the call site with the expansion chain.

## 7. Performance design

- **Cache key:** `hash(normalized function IR, own contract, callee contracts, policy, prover version)`. Normalization alpha-renames locals and drops source positions, so the 10 000 identical-shaped functions in a generated file are proved once.
- **Scheduling:**
  - obligations are independent;
  - functions are scheduled over the pthread pool in bottom-up call-graph order when summaries are inferred;
  - with explicit contracts, all functions run in parallel with no ordering constraints.
- **Incremental:** editing a function body re-verifies only that function, unless its contract changed.
- **Budgets:** T1 widening is fixed and deterministic, T3 uses fuel. Exhausting the budget gives `runtime-check` in checked mode and an error in strict mode that says *budget* (not *false*), with the remaining goal attached.

## 8. Forks to decide

| # | Fork | Options | Leaning |
|---|---|---|---|
| 1 | Memory model | M2 permission modes (T2, fast) / M1 formulas only / M3 user predicates | M2 now, M3 later as an extension |
| 2 | Spec builtin namespace | `$name` / contextual keywords (`old`, `forall`) / `__spec_name` | `$name` |
| 3 | Range syntax | `p[lo:hi]` half-open / `p[lo:+len]` / Cilk `p[start:len]` | `p[lo:hi]` |
| 4 | Canonical spelling | bare `#requires` + pragma form / `#pragma cereal` only / a single `#spec <clause>` directive | bare directives canonical, pragma for portability, `#if $spec` blocks |
| 5 | Summary inference | infer contracts for unannotated functions (T1 bottom-up) / require contracts at boundaries | infer, show in the LSP, one-click insertion |
| 6 | Policy granularity | per UB class (§6.3) / per function only | per UB class |
| 7 | Spec integer semantics | mathematical / C semantics | mathematical |
| 8 | Checked-mode pointer metadata | low-fat pointers / fat pointers (ABI change) / shadow memory | low-fat |
| 9 | Unbounded quantifiers | ghost-only / allowed in strict with fuel | ghost-only |
