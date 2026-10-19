---
status: draft
---

# Typed AST export (TAST)

Track D prerequisite for both the back end (ROADMAP: typed-AST export, SysV
classification, QBE) and VeriFast-style proving (CONFIG_DESIGN direction,
Round 238). Built through stage T7 and the statement half of T8 (2026-10-18): see "Stage map as built"
after the section 5 table; each stage has its own `docs/TAST_T<n>_PLAN.md`.
The lowering to the core (the old T6 row) is `TAST_T8_PLAN.md`: T8a (statements to a CFG) and T8b (expression control flow, with the execution oracle) are built.

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
   default argument promotions for K&R, NOPROTO and variadic calls, and the
   NOPROTO types of hand-checked builtins), entry conversion of K&R-defined
   parameters, implicit function declarations (an `int()` callee), null
   pointer constants, `_Bool` conversion (a nonzero test, not truncation),
   casts to `void`, conditional-operator result type, shifts, and the
   conversion of `case` values to the promoted `switch` type (`cstmt.c:567`).
2. **Value category** (`lvalue`) lives in `ef` bits. On expression nodes
   `ty/cv/ck/ef/cb` are written once, in the node's own visit (exceptions:
   `EF_NPC` is OR-ed in last, `EF_FOLDWARN` is cleared); the "reused per tag"
   caveat of TYPES.md applies to declaration nodes. `EF_BFPROMOTE` is set in
   `e_member` (`ccall.c:2320`) on the member node and read by `promoted()`;
   it survives to unit end, so `promoted()` is callable there. (Round 4
   wrongly called it a call-site bit; round 5 corrected it.) Open: an
   assignment to a bit-field has `ef = EF_SIDE` and loses the flag
   (`cexpr.c:6745`). Probe (2026-10-18, gcc-13 `-std=c11`, `_Generic` on 20
   bit-field expressions, `int a:3; unsigned b:5; unsigned char c:3;
   long long d:33; unsigned long long e:40`): gcc gives a bit-field lvalue
   its narrow type for every one (matches none of `int/unsigned/long
   long/unsigned long long`, so `default`), and the same for the value of an
   assignment, `x++` and `x += 1`; but `a + 1`, `b + 1`, `c + 1`, `+b` and
   `?:` of two narrow fields promote to `int` (widths below 32), while `d + 1`
   and `e + 1` stay narrow. cereal reports the declared type for `a, b, d, e`
   and for the assignment, `++` and `+=` values, and `long long` for `d + 1`
   and `e + 1`. So the checker differs from gcc in `_Generic`/`typeof` of
   bit-field expressions (rare; parked in PARKED.md). For TAST the rule is:
   a bit-field load or store carries `Field.width` and `off_bits`, and an
   assignment's value is the stored (truncated) value, never read from `ty`.
3. **Symbol binding of identifiers is not stored for locals.** `cb` is set
   in `e_ident` only for globals, statics, externs and linkage objects;
   automatic locals and parameters get none. The binding is a scope lookup at
   visit time, and every scope is popped before unit end (`check.c`, before
   `csx_unit_end`), so it cannot be redone afterwards. The symbol index
   records it (`csx_sym`) only when `Checker.sx` exists. An identifier's `ty`
   can also differ from its symbol's type.
4. **Initializers** are checked, not normalized: designators, brace elision
   and zero fill are not turned into a flat (offset, value) list. A string
   literal initializing a `char[]` is an array copy, not a decay.
5. **String literals** are token pieces (`node_pieces`); the decoded bytes,
   element type (`char`, `wchar_t`, `char16_t`) and terminator are not kept.
   `__func__` is a node reference (`CB_NODE|i`), not a symbol.
6. **Constants.** `cv` is `uint64_t` and floats are host `long double`
   (`fv`), two slots for complex. `__int128` values, `_Float128` and the
   decimal types do not fit; the target's long double format is not the
   host's. The TAST `value` field cannot claim "folded constant" for these
   until a representation is chosen (stage T4).
7. **Statement structure** is complete in the tree, but `goto` targets are
   tokens, not label nodes. `switch` case values are readable (`cv/ck` of the
   case expression nodes).
8. **Unevaluated and dead operands** (`sizeof` of a non-VLA, `_Generic`
   non-selected arms): the tree has them; a consumer must not emit them.
   TAST drops them. A VLA `sizeof` operand is evaluated and is kept.
9. **VLAs, compound literals, statement expressions, `__builtin_*` (some
   have no function type), `asm`, `va_arg`, `offsetof`, vector and complex
   operators, bit-field load and store, `a ?: b` (evaluate `a` once)**:
   present as nodes; their semantics for a consumer are undefined until
   listed (section 5).
10. **Option dependence.** Under `-O` the checker replaces constant variables
    (`fold_const_var`, `cexpr.c`), so the folded values depend on options;
    TAST is defined for the default `-O0` check only.
11. **Macro provenance** is per token (`PTok.exp/mloc`); a TAST node needs
    one SrcLoc (the expansion point, as diagnostics use).
12. **Attribute arguments** (`IDENT` under `ATTR_ITEM`) are not expressions
    and have no type; the walk skips them.

## 4. Design

**One mechanism: an elaboration pass from (nodes + side arrays + symbols +
types) to a TAST, run at unit end, driven by a per-tag rule table.**

- Placement: a new `src/c/tast.[ch]`, called next to `csx_unit_end` when
  `Checker.tast` is set. A unit is skipped, and the dump says `skipped:
  errors`, when it had any error: a syntax error (`had_errors` in
  `checker_unit`, which is not kept in the Checker today, so the pass keeps
  it; `Checker.quiet` is not enough, a function definition with a syntax
  error is not quiet) or an error-level diagnostic during the unit (the
  `diag->nerrors` delta, as the S0 probe used; pragma-suppressed errors
  still count as errors there). A type error leaves error types in `ty`.
- **Bindings are recorded during the visit, not rebuilt.** Section 3.3 shows
  scopes are gone at unit end, so a small side array `sym[i]` (symbol ref
  of each IDENT/declared node) is filled where `e_ident` and the
  declaration code already resolve names; the symbol index's `csx_sym`
  already makes the same call and is the model. This is the one place the
  design touches the checker's visit, and it is a store, not a decision.
- The pass does not re-derive types. It reads `ty[i]` and `cv/ck[i]` the
  checker already computed and inserts conversion nodes where an operand's
  type differs from the type the operator requires. The required type comes
  from the same functions the checker used (usual arithmetic conversion,
  promotion, decay, assignment compatibility) called, not copied; some are
  already separate helpers (`common_type()`, declared `cexpr_int.h:357`,
  defined `cexpr.c:62`, takes TypeIds only; `promoted()`, `cexpr_int.h:225`,
  reads `ef`, so it is limited by section 3.2). Where a needed function is
  private and fused with warning logic, the fix is to split it in the
  checker (one place), not to duplicate it. Dropped qualifiers: `rvt()`
  returns the unqualified type, which is the rule for every load.
- The walk skips `ATTR_ITEM` subtrees (section 3.12).
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
  couples every expression rule to the exporter. **Decided by the S0
  site inspection (section 11): a hybrid.** Derive at unit end every site of
  3.1 except two, which are recorded during the visit: symbol bindings
  (`sym[i]`, above) and, for initializers, per scalar value node the target
  subobject type, byte offset and bit-field width/shift, written where
  `digest()` (`cinit.c:1063`) holds the designator and brace-elision cursor.
  Two optional records remove fragilities: the compound-assignment operation
  type (`e_arith` sets it, `e_assign` overwrites it) and the default-promoted
  type of an argument to a NOPROTO or variadic callee (the checker never
  computes it). Both are also recomputable (`common_type(promoted(l),
  promoted(r))`; float to double, else `promoted()`), so they are not needed
  unless a test finds a disagreement.
- **End state: a small core, reached by desugaring (decided 2026-10-18,
  section 13).** The rule table (conversions) and the shape builders are
  not two designs: the builders are sugar over a core of `const sym load
  store addr unop binop cast call cond` plus `label goto branch`. The shape
  lowerings (pointer arithmetic, `p->f`, `a[i]`, compound assignment,
  `++`/`--`, `&&`/`||`/`?:`, comma) become entries of one *template table*
  (sugar tag, template over core ops), applied by one generic loop, instead
  of per-tag C code. Loops and `switch` lower to `label`/`branch` in a
  separate, additive second pass (stage T6); the structured form stays the
  first stage, so T1 to T5 are unaffected. The T6 form is the control-flow
  graph that Track D's dataflow warnings and symbolic execution need.
  Cannot be expressed as templates (explicit rules): argument-typed
  builtins, `asm`, VLA allocation, initializer layout.
- **Identity shared with the symbol index (decided 2026-10-18).** A TAST
  `sym` node carries the index's identity key, computed by the same shared
  function (`cindex_key`, B5_DESIGN E10, 5.3), alongside the checker's
  `SymRef`. Only the key is shared, not the index's event stream (which
  exists only with `Checker.sx` and has macro-body and expansion rules a tree
  does not want). The key makes TAST, hover, references, call hierarchy and
  a back end agree on what an entity is by construction.
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
| T1 | integer, enum and `_Bool` scalars; locals; assignment, compound assignment, `++`/`--`, comma, `&&`/`\|\|`/`?:` (incl. `a ?: b`), casts including to `void`; `if/while/for/do/switch/goto/labels/return`; calls to prototyped non-variadic functions with scalar arguments; non-VLA `sizeof`/`alignof` folding | cast-target multisets |
| T2 | pointers, arrays, decay, pointer arithmetic, function designators and pointers, `.`/`->`, structs and unions (by value too), bit-fields (load, store, truncation), string literals | multisets + layout regression |
| T3 | initializers (flat list), static data, implicit and NOPROTO calls, K&R definitions, variadic calls and default promotions, `case` value conversion | multisets + data golden |
| T4 | floating point, `long double`, `_Complex`, `__int128`, decimal and `_FloatN` (needs the constant representation of 3.6) | multisets |
| T5 | VLA (incl. evaluated `sizeof`), compound literals, statement expressions, `_Generic`, `va_arg`, `offsetof`, computed `goto`, argument-typed builtins, `asm` | decided per item |
| T6 | lowering to the core: templates for the shapes of section 4, loops and `switch` to `label`/`branch` (a CFG per function) | additive; the structured dump stays; a CFG verifier (every block reachable or marked, every branch target defined) |

### Stage map as built (2026-10-18)

The table above is the original plan; the stages that were built split its T5
row and renumbered. Rows are the plan files; the old lowering stage is
now T8 (`TAST_T8_PLAN.md`).

| Built | Content | Plan |
|---|---|---|
| T1, T2 | as in the table | `TAST_T1_PLAN.md`, `TAST_T2_PLAN.md` |
| T3 | initializers, static data, calls, K&R and variadic definitions, statement expressions, `_Generic`/`offsetof` (folded by the checker), overridden initializer elements | `TAST_T3_PLAN.md` |
| T4 | `float`/`double`/`long double` and `fconst`, `va_*` | `TAST_T4_PLAN.md` |
| T5 | evaluate-once temporaries (`tmp`) for compound lvalues with side effects | `TAST_T5_PLAN.md` |
| T6 | computed goto (`labaddr`, `igoto`) | `TAST_T6_PLAN.md` |
| T7 | compound literals (`complit`) including static ones folded into initializers, sized atomics, flexible array members | `TAST_T7_PLAN.md` (atomics and flexible arrays: commits `d70a407`, `742484f`) |
| T8a | statements lowered to a control-flow graph (`--dump-cfg`, `--verify-cfg`, `tcfg.c`) | `TAST_T8_PLAN.md` |
| T8b | expression control flow (`&&`, `\|\|`, `?:`, comma, statement expressions) lowered with `reg` and spills; execution oracle `tests/cfg_exec.py` | `TAST_T8_PLAN.md` |
| spike | x86-64 back end for the integer/pointer/array subset: CFG to MIR (`src/c/mir.[ch]`, machine-instruction list with an AT&T text printer) via `src/c/cgx86.c`; `--dump-asm`; native oracle `tests/asm_exec.py` (assemble, link, run against gcc; integer and float/double functions, SysV register and stack arguments). Unsupported functions are reported as `# x86: name unsupported: why`. `--emit-obj=FILE` writes an ELF64 object from the same MIR (`src/c/mir_elf.c`); its .text is compared byte for byte with `as`, and the object is linked and run | `TAST_T9_PLAN.md` decision 4 |

Still unsupported: VLAs, `_Complex`/`__int128`/`_FloatN`/decimal types, a bit-field
compound lvalue with a side effect, `asm` and implicit-declaration calls
(`PARKED.md`). Corpus: lua54 0 unsupported units, libuv 2 (`asm`,
`setproctitle`).

Because an unsupported construct drops the whole unit, a real corpus function
elaborates only once every construct it uses is in. Expect near-zero
coverage on lua54 and libuv until T3, and report the per-construct counts
of `unsupported` reasons at every stage, so progress is visible before
coverage moves.

## 6. Verification

- **Types and conversions against gcc.** For each corpus function, normalize
  `-fdump-tree-original` (gcc 13) and the TAST dump to a common form and
  compare per function. gcc folds, reorders and names temporaries, and
  casts print their target type only (section 11), so the comparison is
  the *multiset of cast target types* per function, not text equality and
  not operand types; mismatches are listed, with an allow-list for known
  gcc folds. This is a coarse differential, not a proof, and it says
  nothing about arithmetic conversions that gcc prints no cast for.
- **Internal invariants (a `tast_verify`, like `cindex_verify`).** Every
  expression node has a non-error type; no `lvalue` appears where an rvalue
  is required; every `sym` resolves; statement nesting is well formed; node
  ids are in post-order; the dump parses back to the same structure (round
  trip). A check that operand types equal what the operator requires would
  use the same rule table that built the tree, so it proves only that the
  pass ran, not that the rules are right; the independent evidence is the
  gcc comparison above, the layout check, and, once a back end exists,
  running compiled programs against gcc's.
- **Layout.** `tests/gen_layout.py` already asserts the checker's `Record`
  layout against gcc. TAST reads that same `Record`, so this is a
  regression guard on the checker, not independent evidence about the
  TAST; the TAST dump must only be checked to print what `Record` holds.
- **What is independent, honestly.** Until a back end exists, only the
  cast-target multisets and the non-circular invariants (typed, resolved,
  well formed) test the elaborator. Evidence for arithmetic conversion,
  argument promotion and bit-field truncation arrives with compiled-program
  comparison, which is outside T1 to T5.
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
unverified and was wrong in kind (round 4: case expression values are readable in cv/ck; the claim was wrong the other way).
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

## 11. Spike results (2026-10-17, throwaway instrumented copy, removed; the numbers are not reproducible, the probe was deleted)

**S0, partial (type presence only).** At unit end, on lua54 (all 33 units) and libuv (src and
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
declared types, by reading), and `EF_NPC` use. Verdict (reviewed down, round 4): type presence holds; whether the unit-end design
holds for conversions is open (section 4); alternative B is
not ruled out. Bindings of locals are known to need the visit (section 3.3).
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

Round 4 (fresh-eyes review by a Sonnet subagent, claims re-verified by hand
for 1 to 4): (1) `cb` links only globals and linkage objects, locals have no
symbol link and scopes are gone at unit end, so bindings must be recorded
during the visit (sections 3.3, 4); (2) `quiet` is `had_errors && !FUNC_DEF`,
so a function with a syntax error is not quiet, the skip rule now uses an
error count; (3) case values are readable and what is missing is their
conversion to the promoted switch type; (4) S0 measured type presence only
and is relabelled partial, the viability verdict is withdrawn, ROADMAP says
"S0 partial"; (5) `promoted()` depends on a call-site `ef` bit (wrong, fixed in section 12); (6) 128-bit,
float128 and decimal constants do not fit `cv`/`fv`; (7) the stage table had
dependency inversions (enums in T3, variadics in T1, bit-fields after
structs) and omitted `&&`/`?:`/assignment/goto, rewritten with an expected
near-zero coverage until T3; (8) added K&R entry conversion, implicit
declarations, `a ?: b`, void casts, string array copy, `-O` option
dependence; (9) VLA `sizeof` is evaluated; (10) the layout check only
guards the checker, said so; (11) section 6 aligned with what S1 found.
Round 5 (site inspection, section 12): round 4 item 5 was itself wrong (`EF_BFPROMOTE` is set in `e_member` and survives), corrected; conversions are derivable except initializers, so the verdict is a hybrid. The lexer does skip comments and emit no token (`lex.c:1317`), so section 8(b) stands.
Round 4 note: the gcc examples in section 2 were checked by me in WSL; the lexer claim of section 8 was checked in round 5.

## 12. S0 site inspection (2026-10-18, by reading; a Sonnet subagent, key claims re-checked by hand)

For each conversion site of 3.1: where the checker decides it, whether its
inputs survive to unit end, and whether a pure helper computes the target.

| Site | Derivable at unit end | Helper |
|---|---|---|
| Integer promotions (unary, binary, compare, `?:` arms) | yes; `ty[i]` is already the promoted or common type | `promoted()` |
| Usual arithmetic conversions | yes; `ty[i]` is the operand target. Compound assignment: `e_assign` overwrites it, recompute | `common_type()` (`cexpr.c:62`) |
| Shifts | yes; result is `promoted(left)`, the count is not converted | `promoted()` |
| Array and function decay | yes, but context-driven (not under `&`, `sizeof`, `alignof`, `typeof`, a subscript base, a string initializing a `char[]`); nothing is recorded, read `par[]` and the tag | `rvt()` gives the type only; the "decays here" test is not factored out |
| Lvalue-to-rvalue | yes; `EF_LVALUE` plus the parent's role | `rvt()` |
| Assignment, argument, return | yes; return type via the declarator under `N_FUNC_DEF` (`func_sym` is transient but the declarator keeps `cb/ty`) | none returns the converted type; fused with the warnings |
| Initializer | **no**: the designator/brace-elision cursor is transient and values are not built | fused; simple `T x = e;` is derivable |
| Prototyped vs NOPROTO/variadic arguments | yes, from the callee type; the checker does not compute default promotions, compose them | `type_default_promote` lacks bit-fields |
| Null pointer constant | yes; the `ef` bit persists (baked in under `-O` rules) | `is_npc` |
| Conversion to `_Bool` | yes; warnings only, no type is computed | trivial |
| `?:` result | yes; `ty[i]` is the result and every non-void arm converts to it | `common_type`, `type_composite` |
| `case` value to switch type | yes, recompute from the switch condition (uses `type_int_promote`; verified 2026-10-18 equal to `promoted()` for enums, which only adds the bit-field case, and equal to gcc on `_Generic(+e)` for unsigned, signed, 0x80000000 and 64-bit-underlying enums) | inline in `cstmt.c:567` |
| Comparison | yes; result `int`, operands converted to the common type (recomputed, not stored) | `common_type` |
| Unary `+ - ~ !` | yes; `ty[i]` is the operand target (`!` gives `int`) | `promoted()` |

Not audited: the per-builtin custom checks in `ccall.c` (about lines
773-2220); whether `lsyms` are still valid at unit end for the builtin
lookup. Unsure: bit-field assignment value type.

Consequence: the S0 gate is met with the hybrid of section 4; T1 needs the
`sym[i]` recording and (from T3) the initializer recording.

## 13. Generalizations considered (2026-10-18)

Asked: are there unifications the design misses? Four were found; two are
adopted, two parked (PARKED.md, section "TAST generalizations").

1. **Desugar to a small core with a template table. Adopted** (section 4,
   stage T6). The design had two mechanisms, a conversion rule table and
   per-tag shape builders. All the builders are sugar over a few core ops,
   so one template table plus one loop replaces them; lowering loops and
   `switch` to `label`/`branch` adds a CFG that Track D (dataflow warnings)
   and symbolic execution (proving) need, as an additive second pass, so
   T1 to T5 are unchanged. Subsumes: pointer arithmetic, `->`, `[]`,
   compound assignment, `++`/`--`, `&&`/`||`/`?:`, comma, `while/do/for`,
   `switch`. Cannot: argument-typed builtins, `asm`, VLA allocation,
   initializer layout. Cost: the lowered tree is not 1:1 with the source;
   the structured form is kept first.
2. **Share identity with the symbol index. Adopted** (section 4): `sym`
   carries the shared `cindex_key`, not the event stream. Cost: none beyond
   calling the key function with a TAST symbol; the index needs `Checker.sx`
   and a tree must not.
3. **One annotation log from the visit** (BIND, CONV, INIT records through a
   single `ann()` API) instead of separate `sym[i]`, initializer records and
   the two optional recomputable records. **Parked:** it only pays when a
   third recorded item appears; each record is one store at a decision
   point today.
4. **Run the warnings over the elaborated tree** (`-Wconversion` as "for each
   cast node, is it unsafe", deleting the fused type-and-warning logic in
   `cconv.c` and the assignment checks). **Parked:** large and risky; the
   gcc goldens (about 3,600 diagnostics cases) would make it testable, and it
   follows once conversions are explicit nodes.

Review round 6 (this change): the T6 row's "additive" claim was checked
against the T1 plan (no T1 task depends on the lowered form); the shared key
was checked to exist as a single function (`cindex_key`, B5_DESIGN E10); open:
whether `cindex_key` accepts a symbol with no index events (a local or a
static in a unit without `sx`), to be confirmed at T1 task 2 before `sym`
carries it.

## 14. T1 results (2026-10-18)

T1 (tasks 1 to 9 of TAST_T1_PLAN.md) is built: `src/c/tast.c`, `--dump-tast`,
`--verify-tast`, goldens in `tests/tast` (plain and from cells), the gcc
comparison `tests/tast_gcc.py`.

- **Shape.** One recursive pass over the syntax tree reads `ty/cv/ck/ef/sym`;
  conversion targets come from `promoted()` and `common_type()`; each
  arithmetic result type is compared with the checker's own and a mismatch
  makes the unit `unsupported: rule disagreement` (none seen). A constant
  operand converts by folding in place; an explicit cast to the same type
  leaves no node. Two nodes are not in the core: `post` (value of `x++`, the
  old value of its store) and `elvis` (`a ?: b`, evaluates `a` once); both
  lower with a temporary in the lowering stage (the evaluate-once `tmp` of
  T5 is a different temporary: it names an lvalue's address, not a value).
- **Local declarations** are a `local` node whose optional kid is the
  initializer converted to the declared type (the plan said `local` plus a
  `store`; the single node is simpler to verify). A declaration's symbol is
  `cb[DECLARED]`, so `Checker.sym` is needed only for identifier uses.
- **Verifier.** Kids precede parents; every node but the root is used once;
  values are never bare lvalues; operand types match the operator; statement
  and expression positions are kept apart; every `goto` has a label. It
  prints `verify:` lines and exits 1 on a finding (checked by breaking an
  operand conversion); the plan's "dump parses back" check was not built (the dump is not parsed anywhere).
  A deliberate break (an unconverted operand) is reported, so it does check.
- **Coverage** (run `2026-10-18-tast-t1-coverage-b`, WSL, option on vs off
  wall time): lua54, 30 functions elaborate of 2,243 units with a body (1.3%),
  unsupported reasons: pointer type 2,100, global initializer 69, floating
  type 28, unprototyped or variadic definition 10, expression INDEX 4,
  static or extern local 2. libuv: 21 functions elaborate, 1,542 unsupported:
  pointer type 1,476, struct/union type 20, global initializer 15, unary `*`
  9, call to unprototyped or variadic 7, static or extern local 5. No
  verifier findings on any elaborated unit. As expected, pointers (T2) are
  the wall; the count of functions that elaborate becomes meaningful after T2
  and T3.
- **Cost.** Option off: callgrind 3,887,604,577 against about 3.89G in the
  gate header (no change). Option on, 35 lua54 files: 0.83 s against 0.54 s,
  most of it printing the declarations of the user headers once per
  translation unit.
- **gcc comparison** (arith, assign, flow): 19 of 26 functions have the same
  multiset of cast target types. All seven differences are the front end's
  own folding in gcc, not errors in the tree: `shorten_binary_op` computes
  `unsigned + long` stored to `unsigned` in `unsigned` (`ul`), a sum
  converted to `int` is computed in `unsigned` (`fold`, `mix`), `c ? a : b`
  converted to `int` casts each arm instead (`cnd`), `_Bool c = b` prints
  `b != 0` (`bl`), `x++` and `x += 300` on `unsigned char` print without
  their conversions (`inc`), and `-a + ~a + +a` is simplified (`neg`). The
  oracle therefore stays a lead generator (as section 6 said), not a gate.
- **Decisions confirmed.** The enum `switch` case conversion equals gcc's
  (probe in section 12's table). `unit_err` (parser flag or `nerrors` delta)
  skipped 8 libuv units, none in lua54.

## 15. T2 results (2026-10-18)

T2 (docs/TAST_T2_PLAN.md) is built in `src/c/tast.c`: pointers, arrays,
decay, pointer arithmetic, function designators and calls through pointers,
`.`/`->`, structs and unions by value, bit-fields, string literals as values.

- **Shape.** Three lvalue nodes (`sym`, `deref`, `field`; `str` for a string
  literal) and `load`/`store`/`addr` over them; `a[i]` is `deref(ptradd(a,
  i))`, `p->f` is `field(deref p)`, decay and `&` are `addr`, a function
  designator is `addr` of a function `sym`. `ptradd` (element size in the
  node, index converted to `ptrdiff_t`) and `ptrdiff` are the only new
  shape lowerings. Conditions and `!`/`&&`/`||` take pointers raw, as they
  take integers. No new rule table: pointer conversions are the T1 `conv()`
  with a pointer target, so a qualification change (`char *` to `const char
  *`) is a `cast` node.
- **Field nodes carry a byte offset, a bit offset within that byte and a
  width** (width 0: a whole member), all derived from `find_field`'s total
  bit offset. `tests/tast_layout.py` compares them with where gcc-13 puts 26
  members (packed, aligned, anonymous unions and structs, zero-width and
  mixed-type bit-fields): 0 mismatches.
- **Compound assignment and `++`/`--` name the lvalue twice**, which is safe
  only without side effects; an lvalue with `EF_SIDE` (other than a plain
  variable) made the unit `unsupported: side effect in compound lvalue`
  (T2 result; T5 added the `tmp` temporary and closed it).
- **Coverage** (run `2026-10-18-tast-t2-coverage`, option on 0.77 s against
  0.62 s off on lua54): lua54, 1,578 functions elaborate (T1: 30), 665 units
  unsupported: builtin or undeclared call 250, floating type 168,
  unprototyped or variadic call 134, global initializer 69, static or extern
  local 30, unprototyped definition 10, side effect in compound lvalue 2,
  `va_arg` 2. libuv: 1,379 functions (T1: 21), 184 unsupported: statement
  expression 99, unprototyped or variadic call 37, static or extern local
  15, global initializer 15, floating type 10; 8 skipped for errors. The
  verifier found nothing on any elaborated unit.
- **gcc comparison** (`tests/tast_gcc.py` on the T2 tests): 32 of 59
  functions have the same cast multiset. The differences are gcc's:
  indexes and pointer offsets are converted to `sizetype` (unsigned) where
  cereal converts to `ptrdiff_t`; qualification and `void *` casts that gcc
  does not print; bit-field loads that gcc prints as casts to the field type.
  Bit-field correctness rests on the layout test and the goldens.
- **Deviations from the plan.** `str` holds only its type (the bytes are not
  exported yet; parked). The gcc differential was not extended to
  `POINTER_PLUS_EXPR`/indirection counts. `deref` also allows a function type
  (`*fp` before decay).
- **Symbol identity, measured (decided 2026-10-18).** With the symbol index on
  (`--dump-symbols`), every `sym`, `local` and direct `call` symbol has an
  index declaration: 34,171 `sym` nodes over the 1,578 lua54 and 1,379 libuv
  functions that elaborate, none missing (run `2026-10-18-tast-t2-key`;
  `csx_decl_of`, the verifier check, prints `name=#N`, the same `#N` as
  `--dump-symbols`). The TAST therefore does not copy `cindex_key`: the
  index's own decl id is the link, and the key is `cindex_key(ix, N)` once
  the index is frozen (the key needs the finished events for its anchor, which
  do not exist at unit end). This replaces the "carry the key" wording of
  section 4: the shared function stays shared, the node carries the id.
