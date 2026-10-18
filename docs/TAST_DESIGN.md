---
status: draft
---

# Typed AST export (TAST)

Track D prerequisite for both the back end (ROADMAP: typed-AST export, SysV
classification, QBE) and VeriFast-style proving (CONFIG_DESIGN direction,
Round 238). Nothing here is built; section 7 lists the spikes that must run
before any code.

## 1. Goal and non-goals

**Goal.** After a clean `check`, hand a consumer one *elaborated* tree per
external declaration in which every expression has its final type, every
implicit conversion is an explicit node, every constant is folded, every name
points at its symbol, and every layout question (size, alignment, field
offset) is answered. A consumer then needs no C typing rules of its own.

**Non-goals.** Code generation, an optimizer IR, SysV classification (a
consumer of TAST, ROADMAP step 2), proving itself, an annotation syntax (see
section 8), a stable external format. TAST is not produced for a unit with
syntax or type errors (the checker runs `quiet` there; section 4).

**Consumers it must serve, so the format is not shaped by one.**
1. Back end: needs types, layout, explicit conversions, lvalue/rvalue
   distinction, control-flow structure, static initializer values.
2. Proving: needs the same plus a stable link back to the source tokens
   (spans), because annotations attach to source positions, not to nodes.
3. Tests: a deterministic text dump, golden-compared, and comparable with
   gcc's own `-fdump-tree-original` (section 6).

## 2. What exists (read from the tree, 2026-10-17)

- `ast.h`: a flat post-order `Node{tag, flags, aux, tok, size}` array per
  external declaration (`ParseUnit`), tokens by unit-relative index with
  presentation locations (`PTok`).
- `check.c`: `visit()` walks it and fills per-node side arrays `ty, cv, cb,
  ck, ef` (+ `par`), sized to the unit, reallocated and reused across units
  (`grow_nodes`, freed with the Checker). Their meaning is per tag and is
  documented at the top of `cdecl.c`; `ef` is reused per tag, so a reader
  must not assume it is clean (TYPES.md).
- `type.c/h`: hash-consed `TypeTable` with `Record`/`Field` (offsets in
  bits, bit-field width), `Enum`, function types with parameter lists, and
  the `Target` (sizes, alignments, long double format, bit-field rules).
- Symbols: `CSym`, persistent (`gsyms`) for externals, per-unit (`lsyms`)
  otherwise; `cb` links a node to a symbol.
- Precedent for a per-unit consumer of exactly this data:
  `csx_unit_end(c)` at the end of the unit loop (`check.c:721`), the symbol
  index, which also runs before the arrays are reused.
- Oracle: gcc 13's `-fdump-tree-original` (checked in WSL: `gcc-13
  -std=c99 -fdump-tree-original`) prints the C front end's GENERIC with
  conversions explicit: `int f(short a, char b){return a+b;}` gives
  `return (int) a + (int) b;`, and `p[1]` on `int *p` gives `*(p + 4)` (byte
  offset), cast to the return type.

## 3. What is missing

The checker is a diagnostics port of gcc's front end, not a lowering. Known
gaps, each to be confirmed by spike S0 before the design relies on it:

1. **Implicit conversions are not nodes.** `-Wconversion` logic (`cconv.c`)
   decides *whether to warn* from operand and target types; the converted
   operand type is not stored. Sites: integer promotions, usual arithmetic
   conversions, array and function decay, lvalue-to-rvalue, assignment and
   initializer conversion, argument and return conversion (including
   default argument promotions for K&R and variadic calls), null pointer
   constants, `_Bool` conversion, conditional-operator result type, shifts.
2. **Value category** (`lvalue`) lives in `ef` bits that are reused per tag.
3. **Initializers** are checked, not normalized: designators, brace elision
   and zero fill are not turned into a flat (offset, value) list.
4. **String literals** are token pieces (`node_pieces`); the decoded bytes,
   element type (`char`, `wchar_t`, `char16_t`) and terminator are not kept.
5. **Statement structure** is complete in the tree, but `switch` case values
   live in the statement checker's per-switch state (`do_case`, cstmt.c), not in a side array a consumer reads; `goto` targets are tokens, not label nodes.
6. **Unevaluated and dead operands** (`sizeof` operand, `_Generic`
   non-selected arms, `__builtin_choose_expr`): the tree has them; a
   consumer must not emit them. TAST drops them.
7. **VLAs, compound literals, statement expressions, `__builtin_*`,
   `asm`**: present as nodes; their semantics for a consumer are undefined
   until listed (section 5, "supported subset").
8. **Macro provenance** is per token (`PTok.exp/mloc`); a TAST node needs
   one SrcLoc (the expansion point, as diagnostics use).

## 4. Design

**One mechanism: an elaboration pass from (nodes + side arrays + symbols +
types) to a TAST, run at unit end, driven by a per-tag rule table.**

- Placement: a new `src/c/tast.[ch]`, called next to `csx_unit_end` when
  `Checker.tast` is set. A unit is skipped, and the dump says `skipped:
  errors`, when it has a syntax error (`Checker.quiet`) or any error-level
  diagnostic from the checker (a type error leaves error types in `ty`).
- The pass does not re-derive types. It reads `ty[i]` and `cv/ck[i]` the
  checker already computed and inserts conversion nodes where an operand's
  type differs from the type the operator requires. The required type comes
  from the same functions the checker used (usual arithmetic conversion,
  promotion, decay, assignment compatibility) called, not copied; some are
  already separate helpers (`promoted()`, `cexpr_int.h:225`). Where a needed
  function is private and fused with warning logic, the fix is to split it
  in the checker (one place), not to duplicate it.
- Rule table: one row per `NodeTag` that is an expression or has an
  expression operand: operand slots with the required-type rule
  (`PROMOTE`, `ARITH(a,b)`, `DECAY`, `RVALUE`, `ASSIGN_TO(lhs)`, `ARG(i)`,
  `RETURN`, `COND(arms)`, `NONE`). One generic loop applies it; it subsumes
  every conversion site of section 3.1. It does not subsume the *shape*
  lowerings, which are a second, small set of per-tag builders: pointer
  arithmetic (scale by element size), `p->f` (base + offset), compound
  assignment, `++`/`--`, `&&`/`||`/`?:` control shape, and `switch`. Those
  are stated as such, not hidden in the table.
- **Alternative B, recorded for S0 to decide between.** Instead of
  elaborating at unit end, make `visit()` record each conversion where the
  checker decides it (a side array `conv[i]`: the type operand slot k is
  converted to). Pros: one place makes the decision, nothing is re-derived,
  transient state (`ef`, null-pointer-constant status, bit-field widths) is
  still live. Cons: it touches the checker's hot path (callgrind gate) and
  couples every expression rule to the exporter. The unit-end design above
  is preferred only if S0 shows the converted types are derivable from
  what survives to the end of the unit.
- TAST node: `{op, ty, loc, flags, kids, value}` where `op` is a small
  closed set (const, sym, load, store, addr, unop, binop, cmp, cast, call,
  cond, logical and/or, comma, member-offset, index-as-ptr-add, and the
  statements), `ty` a `TypeId` (the same table), `value` a folded constant,
  a `SymRef`, or a string id.
- Layout is read from `Record`/`Field`/`Target`, never recomputed.
- Output: an in-memory structure for in-tree consumers, plus a text dump
  (`cereal check --dump-tast`), line-oriented like `--dump-types`, node ids
  assigned in post-order. No binary or JSON format: no compatibility
  promise (CLAUDE.md `kind: experiment`), and the first consumer is
  in-tree.
- Versioning: a `TAST_VERSION` constant printed in the dump header; bumped
  when the dump's meaning changes.

## 5. Supported subset, staged

Each stage ends with a green gate (verify and golden tests unchanged) and
its own goldens. An unsupported construct makes the whole external
declaration (the unit) print `unsupported: <construct>` in the dump and
produce no tree for it; it is counted, never guessed or partly emitted.

| Stage | Adds | Check |
|---|---|---|
| T1 | scalars, locals, `return`, `if/while/for/do`, arithmetic, comparisons, calls to prototyped functions | S1 oracle compare |
| T2 | pointers, arrays, decay, `->`/`.`, casts, `sizeof` folding, structs by value | oracle + layout asserts |
| T3 | initializers (flat list), string literals, static data, enums, `switch` | oracle + data golden |
| T4 | floating point, `long double`, `_Complex`, bit-fields, unions, varargs | oracle |
| T5 | VLA, compound literals, statement expressions, `_Generic`, `__builtin_*`, `asm` | decided per item |

## 6. Verification

- **Types and conversions against gcc.** For each corpus function, normalize
  `-fdump-tree-original` (gcc 13) and the TAST dump to a common form and
  compare per statement: result types and the set of casts. Normalization
  will lose information (gcc folds, reorders, names temporaries), so the
  comparison is by *multiset of (operator, operand types, result type)*
  per statement, not text equality; mismatches are listed, with an
  allow-list for known gcc folds. This is a differential, not a proof.
- **Internal invariants (a `tast_verify`, like `cindex_verify`).** Every
  expression node has a non-error type; no `lvalue` appears where an rvalue
  is required; every `sym` resolves; statement nesting is well formed; node
  ids are in post-order; the dump parses back to the same structure (round
  trip). A check that operand types equal what the operator requires would
  use the same rule table that built the tree, so it proves only that the
  pass ran, not that the rules are right; the independent evidence is the
  gcc comparison above, the layout check, and, once a back end exists,
  running compiled programs against gcc's.
- **Layout.** Field offsets and sizes asserted against
  `__builtin_offsetof`/`sizeof` evaluated by gcc on the same corpus
  (reuse `tests/gen_layout.py`).
- **Coverage.** Report the share of corpus functions at each stage that
  elaborate without `unsupported`; this is the stage exit measure.
- **Cost.** The pass runs only when asked; with it off, `callgrind` must
  stay within the 1% gate. With it on, report the overhead.

## 7. Spikes before code (read-only or throwaway)

- **S0 inventory.** For every expression tag, after a clean check on the
  corpus (`~/corpus`: lupa's lua54, uvloop's libuv, zstd-1.5.7-dl; and the
  gcc.dg files that check clean), count nodes whose `ty` is unset or error,
  and list which conversion sites have no accessible helper and which
  inputs (`ef`, null-pointer-constant status) are already overwritten by
  unit end. Output: a table tag x (type present, converted operand types
  derivable at unit end). This decides how much of section 3.1 needs
  checker splits, and between the unit-end design and alternative B.
- **S1 oracle feasibility.** Write the normalization of
  `-fdump-tree-original` for T1 programs and measure how noisy it is on
  200 functions (gcc folds constants, rewrites `i++`, drops casts of equal
  types). If the noise cannot be allow-listed, fall back to the internal
  invariants plus layout only, and say so.
- **S2 initializer shape.** Prototype the flat (offset, value) list for
  struct/array designators on the gcc.dg initializer tests; compare with
  `-fdump-tree-original` static data.
- **S3 cost.** Size of the TAST in memory and the pass's time on libuv
  against the checker's own time.

## 8. Proving, kept open (not designed here)

What TAST must not preclude: (a) every node carries a SrcLoc and the unit
keeps the token index, so a consumer can map annotations to nodes; (b)
comments are not in the token stream today (the lexer drops them), so an
annotation channel is a lexer/preprocessor task with its own design, listed
as a new item in ROADMAP Track D, not here; (c) a prover needs a memory
model (byte offsets vs field paths); TAST gives both (`member-offset` keeps
the field path in `value`). Anything beyond this is deferred to that
design.

## 9. Open questions

1. Does one TAST per external declaration suffice, or does a consumer need
   whole-TU symbol tables (static functions referenced before definition)?
   Likely the latter for the back end; the `gsyms`/`lsyms` split already
   gives it, to be confirmed in S0.
2. Target: only the host x86-64 `Target` is exercised end to end; `-m32`
   cannot run here (no i386 headers). TAST carries the `Target` pointer and
   asserts on it.
3. Where `ef` is reused per tag, does elaboration need its own value-category
   array rather than reading `ef`?
4. Name: `tast` vs `elab`. Cosmetic.

## 10. Review log

Round 1 (claims checked against the tree): `quiet` means syntax errors only,
not type errors, so the skip rule now names both; `promoted()` already exists
as a helper, noted in section 4; the corpus names were the repo's tarball
directories, now exact; the case-value claim ("in `cv` only if folded") was
unverified and was wrong in kind (they live in per-switch state), fixed.
Round 2 (what could be false or circular): the "one mechanism" claim hid the
shape lowerings (pointer arithmetic, `->`, compound assignment, `++`,
short-circuit and `?:` shape, `switch`), now stated as a second, small set;
the invariant "operand types equal what the operator requires" used the same
table that built the tree and proved nothing about the rules, now named as
such with the independent evidence listed; the unit-end placement assumed the
checker's transient state (`ef`, null-pointer-constant status) survives, so
alternative B (record conversions during `visit()`) is added and S0 decides;
"unsupported" was ambiguous between construct and unit, now the whole unit.
Round 3 (re-read for stale text): no further material finding; open risk
unchanged: the gcc comparison may be too noisy (S1 decides, with a stated
fallback).

## 11. Spike results (2026-10-17, throwaway instrumented copy, removed)

**S0 (type presence).** At unit end, on lua54 (all 33 units) and libuv (src and
src/unix), 8 of 147,847 external declarations were skipped for errors; the
rest are clean. Of about 380,000 expression nodes, every node has a type
except: 4,064 `IDENT` under `ATTR_ITEM` (attribute arguments, not
expressions: the walk should skip them), and 52 callee `IDENT`s with their 22
enclosing `CALL`s, all generic builtins whose type depends on the arguments
(`__atomic_load/store/exchange/fetch_add/compare_exchange`,
`__builtin_va_start/va_end`). `EF_LVALUE` is set on identifiers, members,
subscripts and strings as expected. `common_type()` (`cexpr_int.h:357`) and
`promoted()` exist as separate helpers, so promotion and usual arithmetic
conversions are callable at unit end. Not measured: assignment, argument and
return conversion targets (derivable from the callee's function type and the
declared types, by reading), and `EF_NPC` use. Verdict: the unit-end design
(section 4) is viable for the conversion sites inspected; alternative B is
not needed unless a later stage finds a site whose decision state is gone.
Builtins with argument-dependent types need explicit rules (stage T5, or
`unsupported` until then).

**S1 (oracle noise).** Read, not measured: gcc-13's `-fdump-tree-original`
on lcode.c, lstring.c, ltable.c and lvm.c (8,765 lines, 187 functions) is
statement-structured and prints every conversion as `(target-type) expr`,
but it folds and rewrites: `a[key - 1]` appears as a byte offset with the
constant wrapped to unsigned (`+ 18446744073709551615`), `sizetype`
arithmetic, `D.nnnn` labels and temporaries, initializer lines printed
twice, macros expanded. Casts print the target type only, not the source.
So a text or tree-shape comparison is out; a per-function multiset of cast
*target* types and of arithmetic result types is plausible but weak, and its
agreement rate cannot be measured before a T1 prototype exists. Verdict:
keep the oracle as a coarse per-function check (cast-target multiset), do
not rely on it as the main evidence; the strong evidence is layout asserts
now and compiled-program comparison once a back end exists. Stage T1's exit
criterion therefore reads "cast-target multisets agree on at least N% of
functions, mismatches listed", with N set after the first measurement, not
guessed here.
