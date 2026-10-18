---
status: active
---

# TAST stage T1: plan

Implements stage T1 of `docs/TAST_DESIGN.md` (section 5) and nothing else.
Read that doc first; this one lists the tasks, the dump format and the exit
checks. Facts about the checker are from the tree on 2026-10-18.

## 1. Scope

In: integer, enum and `_Bool` scalars; local and global variables; the
expressions `+ - ~ !`, binary arithmetic, shifts, comparisons, `&&`, `||`,
`?:` (and `a ?: b`), comma, assignment, compound assignment, `++`/`--`, casts
(including to `void`); `sizeof`/`alignof` of non-VLA types (folded);
statements `if while do for switch case default break continue goto label
return`, expression statements, blocks, local declarations with scalar
initializers `T x = e;`; calls to prototyped, non-variadic functions with
scalar arguments and results.

Out (each makes the whole unit `unsupported`, with the reason counted):
pointers, arrays, structs, unions, bit-fields, floating point, `_Complex`,
`__int128`, strings, braced initializers, variadic/NOPROTO/implicit calls,
K&R definitions, VLAs, compound literals, statement expressions, `_Generic`,
`va_arg`, `offsetof`, `asm`, computed goto, builtins, vectors, attributes
that change types. Whole external declarations that contain only
declarations (no function body) elaborate to their declared type only.

Why pointers are out: decay and pointer arithmetic are the first shape
lowerings (design section 4); T1 proves the conversion table and the
statement shapes first. The one apparent exception, a call's callee (a
function designator decays to a pointer), is handled inside the `CALL` row:
only a direct call by a named function elaborates (a `call` node holding the
symbol, no decay node); a call through a pointer or any other callee
expression is `unsupported: indirect call`.

Also out in T1: initializers of globals and statics (static data is T3);
only a local `T x = e;` with scalar `T` elaborates, as a `local` node plus a
`store` of the value converted to `T` (derivable from the declared type, the
S0 inspection's one easy initializer case). Units from system headers are
not elaborated (the dump skips them, as the symbol index does with
`SF_SYSTEM`); user-header units are.

## 2. Tasks, in order

Each task ends with the build clean, `sh tests/run.sh` green (2213 tests plus
the new ones) and the existing goldens unchanged unless the task says so.

1. **Option plumbing.** `--dump-tast` in `src/main.c` usage and parsing,
   `fe_opts.dump_tast` (`frontend.h`), through the checker options to
   `Checker.tast` (`check.h`). It implies `-fsyntax-only` checking and
   prints to stdout after the diagnostics. With it off nothing changes.
2. **Binding record.** A side array `uint32_t *sym` in the Checker, allocated
   in `grow_nodes` with the others, freed with them. In `e_ident` (the
   `ref = lookup_ord(c, id)` at the top, `cexpr.c:1989`) store `c->sym[i] =
   ref`; reset it with the other arrays on node entry (`check.c:630-634`,
   where `ty/cv/cb/ck/ef` are set to error/0). This is the only change to the checker's visit. Gate: the
   callgrind total with the option off stays within 1% (`gate.sh`).
3. **Per-unit error flag.** Keep `had_errors` and the `diag->nerrors` delta
   in the Checker for the unit (design section 4), set before the TAST call.
4. **`src/c/tast.h` and `tast.c`.**
   - Node: `{uint8_t op; uint8_t flags; TypeId ty; SrcLoc loc; uint32_t
     kid0, nkids; int64_t val; uint32_t sym;}` in one growable vector per
     unit, ops as an X-macro list like `NODE_LIST`.
   - The rule table of design section 4, with exactly these rows for T1:
     `PROMOTE` (unary `+ - ~`), `ARITH` (binary arithmetic, bitwise,
     compound assignment), `SHIFT` (left promoted, count promoted
     separately), `COMPARE` (common type, result `int`), `LOGICAL` (operands
     to a truth value, result `int`), `COND` (arms to `ty[i]`, `a ?: b`
     evaluates `a` once), `ASSIGN` (rhs to the lhs's unqualified type),
     `CAST`, `CALL` (arguments to the parameter types), `RETURN` (to the
     function's return type, found through the `N_FUNC_DEF` declarator),
     `CASE` (value to the promoted switch type).
   - Shape builders: `++`/`--` and compound assignment as load-op-store with
     the op type recomputed (`common_type(promoted(l), promoted(r))`);
     `&&`, `||`, `?:`, comma, `switch`, `for`.
   - The skip rule and the `unsupported: <reason>` rule, one reason per
     construct, counted per run.
5. **Dump.** One line per node, post-order ids, then one `unit` line per
   external declaration (format in section 3). Header: `tast 1` (the
   `TAST_VERSION`) and the target name.
6. **Verifier `tast_verify`** (run with the dump under `--verify-symbols`'
   style flag `--verify-tast`): every expression node has a non-error type;
   no lvalue where an rvalue is required; every `sym` resolves; node ids are
   post-order and kid ranges nested; statements nest properly; every
   `goto` has a label; the dump parses back to the same structure. Prints
   `verify: ...` lines and exits non-zero on a finding, like `cindex_verify`.
7. **Tests.** `tests/tast/*.c` with `.expected` goldens covering each row of
   the rule table with the awkward cases: `char + char`, `short << long`,
   `unsigned < int`, `enum` arithmetic, `_Bool` assignment and `!x`,
   `a ?: b`, `x++` on `unsigned char`, `x += 1.0` is out (float), a `switch`
   on `char` with a case value that needs conversion and one on an `enum`
   (probed 2026-10-18: `type_int_promote` and `promoted()` agree on enums
   and both match gcc's `_Generic(+e)`; the test pins it), a call through a pointer
   (`unsupported`), a `for` with a
   declaration, a call with a `short` argument to an `int` parameter,
   `return` of a wider value, a K&R definition (expects `unsupported`). Run
   in `tests/run.sh` next to the other golden groups, both default and the
   forced-parallel flags.
8. **gcc differential (coarse).** `tests/tast_gcc.py`: for each test file
   and for a sample of lua54 functions that elaborate, compare the multiset
   of cast target types per function between the TAST dump and gcc-13's
   `-fdump-tree-original`. It reports agreement and lists mismatches; no
   pass threshold until measured (design section 7, S1).
9. **Measure and record.** On lua54 and libuv: the share of units that
   elaborate and the `unsupported` reason counts (expected near zero
   coverage until T3: the point is the reason histogram); the time and
   memory of the pass with the option on; the callgrind delta with it off.
   Raw output through `run.ps1`, a row in `docs/runs.md`, the finding in
   the design doc.

## 3. Dump format (line oriented, as `--dump-types`)

```
tast 1 target x86_64
unit f.c:3:1 func f int(int,char)
  #0 sym a int
  #1 load #0 int
  #2 sym b char
  #3 load #2 char
  #4 cast #3 int
  #5 binop + #1 #4 int
  #6 return #5
unit f.c:9:1 unsupported: pointer type
unit f.c:12:1 skipped: errors
```

Statement ops: `block`, `local`, `exprstmt`, `if`, `while`, `do`, `for`,
`switch`, `case`, `default`, `break`, `continue`, `goto`, `label`,
`return`. Expression ops: `const`, `sym`, `load`, `store`, `cast`, `unop`,
`binop`, `cmp`, `logand`, `logor`, `cond`, `comma`, `call`.

Ids are post-order within a unit; `load`/`store` nodes appear wherever
lvalue-to-rvalue or assignment happens; every implicit conversion is a
`cast` node with the target type; folded constants print as `const <value>
<type>`. Types print with the checker's `type_print`; locations as
`file:line:col` of the expansion point.

## 4. Exit checks

- All tasks' tests green; gate (`bench/tools/gate.sh`) at baseline with the
  option off: tests, verify differences 2/1/0, san 0, callgrind within 1%.
- `--verify-tast` finds nothing on the T1 tests and on every unit that
  elaborates in lua54 and libuv.
- The gcc differential runs and its mismatches are all explained or listed.
- The measured numbers of task 9 are in `docs/runs.md` and the design doc.

## 5. Risks and stop rules

- The `sym[i]` store costs more than 1% with the option off: make the store
  conditional on `c->tast`, and if that still fails, record bindings only
  for declared locals.
- Near-zero coverage on real code means T1 is judged on the tests and on
  per-reason counts, not on coverage; do not widen scope inside T1 to raise
  coverage (that is T2 and T3).
- If the rule table needs per-tag code beyond the listed shape builders for
  T1, stop and update design section 4 before adding it.

## 6. Review log
Round 1 (claims against the tree): the node reset is `check.c:630-634`
(error type, 0, 0, `K_NONE`, 0), so the `sym[i]` reset belongs there, wording
fixed; `e_ident` does open with `lookup_ord` (`cexpr.c:1989`), so the store is
one line. Round 2 (contradictions, gaps): "pointers out" contradicted
calls, whose callee decays, so a direct call is defined as a `call` node
holding the symbol and an indirect call is `unsupported`; global
initializers are static data (T3) and were neither in nor out, now out;
system-header units were unspecified, now skipped; the statement and
expression op lists were missing from the format, added; the enum `switch`
case conversion (`type_int_promote` vs `promoted()`) was unsure in the design
and now has a pinning test. Round 3: no further material finding. Open risk:
the exit check "verify finds nothing on every unit that elaborates in lua54
and libuv" may be vacuous if almost nothing elaborates in T1; task 9
reports the count so this is visible, and the T1 tests carry the evidence.

## 7. Progress
2026-10-18: tasks 1 to 3 done. `--dump-tast` parses and reaches `CheckOptions.tast` (nothing prints yet); `Checker.sym` is allocated and stored only with the option on (so the off path adds one predictable branch in `visit` and `e_ident`); `unit_err` is set at unit end from the parser flag or the `nerrors` delta. Suite 2213 passed; callgrind off-path 3,894,922,073 vs about 3.89G in the gate header (+0.1%). Round 6 open point (does `cindex_key` accept a symbol without index events) is still open for task 4.
