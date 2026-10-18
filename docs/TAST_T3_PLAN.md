---
status: done
---

# TAST stage T3: plan

Stage T3 of `docs/TAST_DESIGN.md` (section 5), built on the T2 pass
(`src/c/tast.c`, `docs/TAST_T2_PLAN.md`, results in design section 15).
Facts are from the tree and the T2 coverage runs of 2026-10-18; anything not
yet read or measured is marked **verify**. Built; results in section 9.

## 1. Why, and what the numbers say

After T2 the unsupported units are (first reason per unit, so these are
upper bounds on what each item unlocks; a unit freed by one fix shows its
next reason):

| Reason | lua54 (665) | libuv (184) | Stage |
|---|---|---|---|
| builtin or undeclared call (`__builtin_expect` 250, rest see 2.1) | 250 | 3 (`__builtin_memset` 2, `setproctitle` 1) | T3a |
| unprototyped or variadic call | 134 | 37 | T3a |
| unprototyped or variadic definition | 10 | 1 | T3a (partly) |
| global initializer | 69 | 15 | T3b |
| static or extern local | 30 | 15 | T3b |
| braced initializer (locals) | 0 | 2 | T3b |
| statement expression | 0 | 99 | T3c (pulled forward from T5) |
| floating type | 168 | 10 | T4 |
| `va_arg`, `asm`, side effect in compound lvalue | 4 | 2 | T5 / parked |
| skipped for errors | 0 | 8 | n/a |

Variadic callees on lua54 are `lua_pushfstring` 36, `luaO_pushfstring` 30,
`luaL_error` 28, `luaG_runerror` 18, `fprintf` 14, `snprintf` 6, `lua_gc` 2;
on libuv `syscall` 9, `snprintf` 9, `fcntl` 5, `sscanf` 4, `ioctl` 4, `open` 3,
`fprintf` 2, `prctl` 1 (run `2026-10-18-tast-t2-coverage-b` and the later
rerun with names). Expected residual after T3: floating point (T4) is the
wall on lua54, roughly 170 units plus whatever the freed units reveal; this is
an estimate, to be replaced by the measured histogram after each sub-stage.

T3 as listed in the design bundles four things of different size and risk.
This plan splits it so each part has its own gate and can stop on its own:

- **T3a calls**: builtins with fixed signatures, variadic and unprototyped
  calls, default argument promotions, `f()` definitions. Small, no checker
  change.
- **T3b data**: one initializer representation used for static data and for
  local aggregates, static and extern locals. The big one; needs the S2 spike
  and a checker hook.
- **T3c `stmtexpr`**: pulled forward from T5 because glibc's `assert` is a
  statement expression (section 2.4). Additive.
- `case` value conversion, listed under T3 in the design, **is already done**:
  `N_CASE` in `src/c/tast.c` truncates the constant to the switch type with
  `cexpr_trunc` (T1). Nothing to do; the design table is stale on this item.

## 2. T3a: calls

### 2.1 Builtins with a fixed signature: one `intrin` node

`__builtin_expect` is special-cased in the checker (`ccall.c`, the
`strcmp(name, "__builtin_expect")` branch: result `long`, constant if the
second argument has an integer value) and is also a row in the builtin table
(`cbuiltin_tab.h:396`: `{"expect", "", 0, "long int|long int|long int", 2}`:
result first, then the parameters). `__builtin_memset` is the row at
`cbuiltin_tab.h:768`. The table is the general mechanism: any row whose
signature is fixed (no `?`, which `ccall_builtin_ptype` already refuses)
yields a prototype `TypeId`, and a call to it is elaborated like a direct
call to a function with that prototype.

Design: a node `intrin` (value op) with `val` = the builtin's index in the
table, kids = arguments converted to the prototype's parameter types by the
existing `conv()`, type = the prototype's result. No per-builtin code in the
pass; the printer shows the table name (`intrin expect`). What `intrin` means
(no side effect other than the builtin's own, result is `a` for `expect`) is
the prover's table, not the TAST's.

Covers: every fixed-signature row, which includes `expect` (250 units),
`memset`/`memcpy`/`strlen`-style rows, and the `__builtin_va_start`/`end`
rows if their signatures are fixed (**verify**: needed for variadic
definitions, 2.3). Does not cover: rows whose signature is argument-typed or
`?` (the design's T5 "argument-typed builtins": `__builtin_choose_expr`,
`__builtin_types_compatible_p`, overloaded atomics), and
`__builtin_constant_p`, which the checker folds (**verify** it is K_ICE
before it reaches the pass; if not, it is the one special case and is said
so).

Open (**verify** first thing): `ccall_builtin_ptype(c, name)` calls
`bt_find(c, name, false)`; the prefixed spelling is handled by
`ccall_is_builtin` (`pre` stripped) but the prototype getter takes the bare
name. The pass needs one entry point that takes either spelling; if the two
differ in how `bt_find` treats the `pre` flag (library-name builtins such
as `memset` have the flag set to 1 in the table, i.e. exist both bare and
prefixed), the semantics of `pre` must be read before use. A plain
`memset(...)` with `<string.h>` declared is an ordinary `CS_FUNC` call and
needs none of this.

Undeclared identifier calls (`setproctitle`, libuv, 1 unit): **verify** what
the checker does. `cexpr.c:1723` implements the C90 implicit declaration;
the pass saw `c->sym[callee] == SYM_NONE`, which suggests an implicit
declaration is not entered as a symbol in this mode. If so it is one more
reason to stay `unsupported` (an implicit declaration is a K&R call to an
unknown function returning `int`; one unit does not justify a node).

### 2.2 Variadic and unprototyped calls

Today any callee with `TF_NOPROTO | TF_VARIADIC` makes the unit
unsupported. T3a handles both with one rule from C11 6.5.2.2: the first
`np` arguments (np = number of declared parameters, 0 for an unprototyped
callee) are converted to the parameter types; every further argument gets
the default argument promotions. In this stage that is the integer
promotions (the T1 `promoted()`); `float` to `double` is T4, so a floating
argument stays `unsupported: floating type` as it is now. Pointers, structs
and unions pass unchanged. An argument of array or function type decays
first (T2 `ex` already does).

Node change: `call` already records its callee form in `pun` (0 direct, 1
indirect). It needs `nfixed`, the number of arguments converted to declared
parameter types; extras beyond it are promoted. The verifier checks: first
`nfixed` kids have the exact parameter types; later kids have a promoted
type (no integer type narrower than `int`, no `float`); for a prototyped
non-variadic callee `nfixed` equals the kid count (the T2 check, unchanged).
A struct returned by a variadic call is the same as a prototyped one.
`nfixed` counts arguments only; for an indirect call the callee stays kid 0
and is not counted.

The reason text changes from "unprototyped or variadic call" to nothing for
these callees, so `tests/tast/unsup.c` loses two lines and gains a
`tests/tast/vcall.c` golden: `printf`-style fixed prefix, `char`/`short`/
`_Bool`/bit-field extras, pointer extras, a struct extra, an unprototyped
`int f();` call with mixed arguments, an indirect call through a variadic
function pointer.

Edge case recorded, not fixed: a call to an unprototyped function whose
definition has a different parameter list is undefined behaviour; no check.

### 2.3 Definitions

- `int f() { ... }` (NOPROTO, empty parameter list, libuv 1 unit): accept.
  The body sees no parameters; callers pass anything (2.2).
- Real K&R definitions (`N_DECL` kids): the corpus has none (the T2 histograms
  list the reason zero times). Stay `unsupported: K&R definition`; parked with
  the trigger "a corpus file uses one". They would need the parameter
  promotion prologue (a `float` parameter is received as `double` and
  converted), which is T4 anyway.
- Variadic definitions (lua54 10, libuv 0): the unit carries a flag
  `variadic` and the fixed parameters; the body reaches the extras only
  through `va_start`/`va_arg`/`va_end`/`va_copy`. `va_arg` is T5 (design
  table), so a variadic definition elaborates only if it never reads an
  extra, which is rare (`luaL_error` does `va_start` and passes the
  `va_list` to `lua_pushvfstring`, no `va_arg`; the `*_pushvfstring`
  family does call `va_arg`). Measure how many of the 10 qualify before
  deciding to carry the flag; if it is 2 or 3 units, leave all of them
  `unsupported` and park. `va_list` on x86_64 is `__builtin_va_list`, an
  array of one struct (T2 `ok_ty` accepts it); as a parameter it decays to a
  pointer.

## 3. T3b: data

### 3.1 One representation for static data and local initializers

The checker does not build an initializer value list: the designator and
brace-elision cursor (`cinit.c`, 2,713 lines; `Lvl` stack, `PSet`/`PEnt`
pending sets, `out_elem` at line 1334, `digest` at 1067) is transient and
decides the target subobject of every value as it goes (design section 12,
"Initializer: no"). In the part of `Lvl` read so far (`cinit.c:78-94`) there
is no absolute byte offset, only indices (`fbase`, `fi`, `idx`, `maxidx`), so
the offset would be accumulated along the stack from field offsets and
element sizes (**verify** the rest of the struct and `RS`).

Design: an **initializer list** is a vector of elements, in source order,

    { byte offset, bit offset, bit width (0 = whole), target type, value node }

attached to a declaration. Static data and local aggregates share it:

- static object (file scope, `static` local, tentative definitions with
  no initializer are an empty list = zero): the unit prints `data <name>=#N
  <type>` followed by the normalized elements; every value must be a
  constant (checked by the checker already; the TAST re-verifies that each
  value tree is built only from `const`, `addr` of a static `sym`, `str`,
  `ptradd` with a constant index, and `cast`);
- automatic object: `decl` gets an `init` list instead of "braced
  initializer: unsupported"; semantics: zero the object, then store each
  element in list order (C11 6.7.9p23: evaluation order is indeterminately
  sequenced, so any fixed order is allowed; source order is the choice).
  A scalar `T x = e;` stays what T1 does today (`decl` plus `store`-style
  initializer); the list is for aggregates and arrays only.

Normalization (done once, when the unit ends, not in the checker): sort by
(offset, bit offset); a later element that overlaps an earlier one replaces
it (designator override, 6.7.9p19; the checker already warns about it);
bit-field elements that share a byte stay separate elements (the image
reconstruction ORs them); unlisted bytes are zero and are not printed.
For an automatic object an overridden element with a side effect is
still evaluated (gcc does), so overriding there keeps both and marks the
first `dead`; for a static one the value is constant and is dropped.
**Verify** against gcc on a `{[0]=f(), [0]=g()}` automatic case.

String literal initializers are elements whose value is the string bytes:
`char s[] = "abc"` (array length derived by the checker, the NUL included),
`char s[3] = "abc"` (no NUL), `wchar_t`, `char16_t`, `char32_t`, `u8`.
This forces the parked item "str bytes": a `str` node must carry its bytes
(and element width), not only its array type. **Verify** where the
checker keeps the decoded bytes at unit end (`__func__` and its two
spellings are made at `cexpr.c:1998` and need the bytes of the enclosing
function's name, section 4; `cinit.c` tracks only the length, `strn`). If
the bytes are not retained, the lexer's token text must be re-decoded by the pass (an escape decoder already exists for the
constant folder: **verify**) rather than adding a second decoder.

Pointer values: an address constant is `addr(sym)`, `addr(str)`,
`ptradd(addr(sym), const)`, `addr(field(deref(addr sym)))` (`&s.f`),
`addr(deref(ptradd(addr arr, i)))` (`&a[i]`), a function name (`addr` of a
`CS_FUNC` sym) or an integer constant cast to a pointer. These are exactly
T2 expression trees, so no new node: the element's value goes through the
same `ex()` + `conv()` as any initializer expression, then a structural
check enforces constness. `(long)&x` as an integer initializer is accepted
by gcc as an extension for some widths; if the checker accepts it, the tree
is `cast(addr sym)` and the structural check above must allow it (**verify**
on gcc.dg).

Out of T3b, kept `unsupported`: compound literals (`&(T){...}` at file
scope, T5), designated initializers of flexible array members in static
objects only if S2 shows the checker's offset logic differs there, VLA
locals, `static` locals of VLA type.

### 3.2 How the elements are recorded: the S2 spike decides

Two ways to get (offset, type, value node) per element:

1. **Record at `out_elem`** (and wherever `digest` finishes a string/scalar
   into the current level): when the pass is enabled (a checker flag like the
   symbol index's `c->sx`; name chosen at implementation), append the element to a per-declaration
   vector with the offset accumulated along `x->stk`. One source of truth:
   designators, brace elision, union member selection, string-into-array
   and the override logic are already solved there. Cost: a hook in a
   2,713-line file that is the checker's most intricate part; must be free
   when off (the callgrind gate is the check: 3,894,075,059 at T2, +0.21% over
   its recorded baseline of 3,886,049,110).
2. **Re-implement the cursor in the pass** from the AST (the T2 `lval`
   style): a second implementation of brace elision and designators, a rule
   the checker owns and the unit-end design avoids duplicating everywhere
   else, so the two can diverge. Rejected unless S2 shows option 1 cannot be
   done without moving `out_elem`'s signature.

Recommendation: option 1. The hook records only what `out_elem` already
holds (value node via `IVal`, target `type`, `key`, `implicit`) plus the
accumulated byte offset; bit-field offset and width come from `find_field`
as in T2 (`tests/tast_layout.py` already covers that arithmetic).

**S2 spike (do first, throwaway, read-only on the checker's behaviour):** an
instrumented copy that prints (offset, bit offset, width, type, value kind)
for every `out_elem` over (a) lua54 and libuv (the 69 + 15 global and the
30 + 15 static-local units, plus libuv's 2 local braced ones), and (b) the
gcc.dg initializer tests that check clean. Report: how many initializers
use designators, brace elision, unions, bit-fields, strings, address
constants, nested arrays of structs; whether offsets accumulate without
reading anything but `Lvl` and the record layout; and the size of the hook
(lines). Also answer: does `out_elem` ever run for a value that is later replaced
or dropped (`Lvl.repl`, `lag`, `has_repl`, `onlyzero`, the pending sets
that mirror gcc's delayed output)? If it does, recording at `out_elem`
captures elements the final constructor does not contain, and the hook must
move to where the pending sets are flushed. Decision rule: if the offset can be accumulated at `out_elem` in
under ~40 lines and without changing control flow, take option 1; otherwise
write up what blocks it before choosing.

### 3.3 Static and extern locals

`extern int x;` inside a function names a file-scope object; the `sym` node
already handles file-scope variables (T2), so the reason today is the pass's
refusal, not a missing mechanism. `static int st;` and `static int t[4] = {..}`
are objects with static storage whose name is scoped to the function: the
function unit grows a `data` record (3.1) printed after its header, before
its body, and `sym` nodes in the body refer to it by the same index id.
The symbol index: **verify** that a function-local static has an index
declaration (`csx_decl_of` returns non-zero); if not, the T2 identity
measurement must be repeated (the check run `2026-10-18-tast-t2-key` counted
only `sym` nodes that existed, and these locals were excluded).
The initialization of a `static` local runs once before the program, never at
the declaration point; the `decl` statement in the body is therefore `nop`
(or absent), and that is how the golden prints it.

## 4. T3c: `stmtexpr`

Libuv's 99 statement-expression units (and the 215 occurrences of
`__extension__ ({` in its preprocessed output) all come from glibc's
`assert`: `((void)sizeof((c) ? 1 : 0), __extension__ ({ if (c) ; else
__assert_fail(#c, __FILE__, __LINE__, __extension__ __PRETTY_FUNCTION__); }))`
(gcc-13 `-E`). Waiting for T5 would leave the largest libuv bucket for no
reason, since a statement expression is needed by no other T5 item.

Design: a value op `stmtexpr` whose kids are the statements of the block
followed by the final expression's value (or none: type `void`); its type
is the type of the last expression statement after the usual conversions
(`void` otherwise). Locals declared inside have the same scoping as a block
(the pass already assigns local ids per function). The verifier extends
"statements are kids of statements" to allow a statement kid under
`stmtexpr` and checks that the final value, if any, is the only value kid
and last. Labels and jumps into or out of the block follow the existing
label/goto checks; a jump into a statement expression is rejected by the
checker (**verify**).

The `assert` expansion also needs: a string for `__FILE__`/`#c` (`str`, bytes
required, 3.1), `__PRETTY_FUNCTION__` (read at `cexpr.c:1998`: `__func__`,
`__FUNCTION__` and `__PRETTY_FUNCTION__` are an identifier node with
`sym == SYM_NONE`, type `const char[len+1]`, lvalue, constant address; the
pass rejects it today as "builtin or undeclared identifier". It becomes a
`str` node holding the enclosing function's name, no static local needed.
Not counted yet: libuv hides it behind the statement-expression reason),
`__assert_fail`
(a prototyped non-variadic function: already fine), `sizeof(c ? 1 : 0)`
(folded). So T3c depends on the `str` bytes decision, not on the
initializer list.

## 5. Order, checks, gates

Order: **T3a** (no checker change, expect ~380+ lua54 units to move on to
their next reason) → **S2 spike** → **T3b** → **T3c** (can swap with T3b if
S2 shows the hook is bigger than expected; it only needs `str` bytes).
Each sub-stage ends green on the standing gate (globals allow-list, `run.sh`
full suite, san, verify differ, callgrind within the recorded band) and
records a coverage histogram run through `run.ps1` with a row in
`docs/runs.md`; the plan's numbers are replaced by measured ones in
`docs/TAST_DESIGN.md` section 16 ("T3 results").

Tests per sub-stage:

- T3a: `tests/tast/{vcall,builtin}.c` goldens; `unsup.c` loses the variadic
  lines. The repo has no negative verifier tests (T2 relied on the goldens and
  the two corpora finding nothing); the new `nfixed` rule is exercised by
  `vcall.c`, and a corrupting debug switch is added only if a rule appears
  that no corpus input can reach.
- T3b: `tests/tast/{init,data}.c` goldens (designators, brace elision,
  unions, bit-fields, strings, address constants, overrides, tentative
  definitions, static locals) and **`tests/tast_data.py`**, the data oracle:
  compile the same file with `gcc-13 -O0 -c`, read each object's bytes and
  relocations (`objdump -s -r`, or `readelf`), reconstruct the image from the
  dump's element list (constants as bytes; `addr` elements as a relocation to
  the symbol plus addend) and compare byte for byte. This replaces the design's
  "compare with `-fdump-tree-original` static data" (gcc prints initializers
  as constructor text and folds them, which the S1 spike found too noisy). It
  checks the one thing a golden cannot: that the cursor arithmetic matches
  gcc. **Verify** that `-O0` keeps unused static objects (gcc's
  `-fkeep-static-consts` is on at -O0 for consts; non-const statics are
  kept when referenced; the test file references them from an exported
  function if needed).
- T3c: `tests/tast/stmtexpr.c` golden, plus the libuv `assert` shape as an
  inline test (no glibc dependence: define the macro in the test).
- Coverage: the same two-corpus run as T2, with the callee-name reasons, into
  the T3 results section.

## 6. Risks and what is deliberately out

- **Hook in `cinit.c`** (T3b): the biggest risk; mitigated by the S2 spike
  and by the data oracle. If it cannot be done cleanly the fallback is to
  leave global initializers `unsupported` and finish T3a and T3c: first
  reasons worth about 390 of lua54's units (250 + 134 + a few definitions)
  and 140 of libuv's (99 + 37 + 4), before their next reasons show.
- **`str` bytes** (T3b and T3c): decides whether the checker must keep
  decoded bytes; **verify** before T3b.
- **Floating point** inside initializers (`static const double t[]`): T4;
  such units stay `unsupported: floating type` and are counted there.
- **Unit-level static data order**: object order in the dump must follow
  source order so goldens are stable; sorted by declaration id.
- **Out**: compound literals, VLAs, `va_arg`, `asm`, argument-typed
  builtins, K&R definitions with declaration lists, implicit-declaration
  calls (unless the **verify** in 2.1 shows they are free), `float`
  promotion (T4).

## 7. Parked by this plan (to add to `PARKED.md` when T3 starts)

- Real K&R definitions (trigger: a corpus file uses one).
- Implicit-declaration calls (trigger: more than a handful of units).
- Overridden initializer elements with side effects, if 3.1's gcc check shows
  a case the list order cannot express.
- Compound literals at file scope (T5).

## 8. Review log

Round 0: draft from the T2 histograms and reads of `ccall.c`,
`cbuiltin_tab.h`, `cinit.c`, `tast.c`.

Round 1 (claims checked against the tree): six corrections. A section
reference pointed at the wrong section; a test-file name (`tast_verify_neg`)
and a design quote ("a second type checker") were invented and are removed;
the fallback value (590 and 100+ units) did not add up and is now 390 and
140; the `__PRETTY_FUNCTION__` claim was a guess and is now read from
`cexpr.c:1998` (a symbol-less identifier, so a `str` of the function name,
not a static local); the callgrind figure is now stated against its
baseline. Also found: `case` value conversion is already done in T1, so the
design table is stale on that item.

Round 2 (what is missing): `nfixed` was undefined for indirect calls
(added); the `out_elem` hook may record values the checker later replaces
(`repl`/`lag` machinery), now a question the S2 spike must answer before
option 1 is chosen. Still open and marked **verify**: `bt_find`'s `pre`
flag, implicit-declaration calls, `__builtin_constant_p` folding, string
byte retention, `-O0` retention of unused statics, static-local index ids.

Round 3 (reread of sections 2.3 to 3.2 for contradictions and unsupported
claims): a doubled sentence in 3.2 reworded; an invented flag name
(`tast_on`) replaced by "a flag like `c->sx`"; the "`Lvl` has no absolute
offset" claim was based on a partial read of the struct (`cinit.c:78-94`)
and is now marked **verify**. Sections 1, 2.1, 2.2, 4 to 7 were not
reread in this round. The open items are reads, not decisions, and none
changes the order T3a, S2, T3b, T3c.

## 9. Progress

### T3a, built (2026-10-18)

`call_args` in `src/c/tast.c` is the one place arguments are converted: the
declared parameters take the parameter type, the rest the default
promotions (`promoted()` on the checker node, so bit-fields and enums are
right; a floating extra is still `unsupported: floating type`). `call` and
the new `intrin` node record `val` = number of declared (fixed) arguments
and `width` = 1 for a variadic or unprototyped callee; they print as `call
f fixed N` (only then). `intrin` is a call to a `__builtin_X` that has no
symbol: `builtin_ft` takes its prototype from the checker's table
(`bt_find(name + 10, true)` and `bt_func_type`), refusing `gnu == 3` (typed)
and `?`/missing signatures. `sym` holds the identifier id of the builtin
name, so the verifier re-resolves the prototype by the same path. The
verifier checks fixed-argument count, argument count, exact parameter
types, and that extras are promoted (no `_Bool`, nothing narrower than
`int`).

Result (run `2026-10-18-tast-t3a-coverage`; `unsup.c` lost its two
variadic units, `vcall.c` added): lua54 1,942 functions elaborate (T2:
1,578), 301 units unsupported (665): floating type 188, global initializer
69, static or extern local 30, unprototyped or variadic definition 10, side
effect in compound lvalue 2, `va_arg` 2. libuv 1,413 (1,379), 150 unsupported
(184): statement expression 102, static or extern local 15, global
initializer 15, floating 12, braced initializer 2, one each of definition,
`asm`, `va_arg`, `setproctitle` (undeclared). 0 verifier findings. Suite
2238 passed, 0 failed. Option-on cost lua54 0.63 s against 0.57 s off.
Not rerun: the callgrind and san legs of the gate (the pass is off by
default; run the full gate before tagging).

Plan check: the round-1 estimate said about 384 lua54 units would move; 364
did. `__builtin_memset` and the 134 variadic units needed nothing beyond the
rule above. The `__builtin_constant_p` and `bt_find` `pre` verifies are
closed: the callee path only takes a name starting `__builtin_`, so
`pre` is always true. Implicit-declaration calls stay unsupported (one libuv
unit).

### S2 spike, done (2026-10-18; throwaway copy `~/cereal-spike`, removed)

A print hook before the pending-set branches in `out_elem`
(`cinit.c`, after `digest`), with the offset summed along the `Lvl` chain
(`Field.off_bits` for records and unions, index times element size for
arrays; the top level uses `key`, ancestors their current `fi`/`idx`).
Hand-checked against the layout of a nested test (arrays of structs,
anonymous struct, bit-fields, union, string, pointer): every offset right.

- **The hook is small and the arithmetic is simple**: about 25 lines for the
  offset; `Field` already carries `off_bits`, no `Lvl` change. Option 1
  (record at `out_elem`) is taken.
- **`out_elem` is also called for closed sub-aggregates** (`v.kind ==
  V_CTOR`, 444 of 3,108 corpus records, plus an implicit repeat when an
  enclosing level pops). Those are not elements: record only `V_EXPR` leaves
  (2,664). `V_ZERO` needs no record.
- **A union override must be an event, not an overlap rule.** At the `LV_UNI`
  branch (`nelems > 0`, "initialized field overwritten") the earlier members
  are discarded as a whole (`L->nel = 0` / `ps->n = 0`); a later shorter
  member does not leave the earlier one's high bytes. The record stream
  therefore needs `clear [offset, offset + union size)` emitted before the new
  element; with the hook before the branches the order was reversed, so the
  hook goes in three places (the two pending branches, and after the union
  branch). The corpus has 0 union overrides, `tests/check` has 3.
- **Strings do not pass through `out_elem` when they initialize a whole char
  array** (`char t[] = "hello"`, `char s[4] = {"ab"}`): the first goes through a
  separate path, the second sets `Lvl.repl`. A second hook where `repl` is
  consumed in `pop_level`, and one for the brace-less string, are needed;
  a string leaf as an element of a larger aggregate (`{..., "ab", ...}`) does
  reach `out_elem` with `v.str` and `strn`. Pointer-to-string (`char *p =
  "x"`) is an ordinary K_ADDR leaf.
- **Out-of-order designators** reach `out_elem` in source order (records are
  not delayed by the pending sets), so sorting plus last-wins on equal
  offsets is enough for leaf scalars; sub-aggregate merging (`.u.a`, `.u.b`)
  is leaf-level too.
- **Corpus shapes** (lua54 + libuv, all constant): 2,664 leaves, 1,382 integer
  constants, 1,274 address constants (647 of them string pointers), no
  bit-fields, no side effects, no designators-with-overrides, no union
  overrides, depth at most 2; level kinds: 899 in records, 1,755 in arrays,
  10 in unions. `tests/check` (79 of 620 files have initializers): 1,348
  records, 7 bit-field leaves, 2 with side effects, 34 in unions, 3 union
  overrides. The corpus does not exercise designators, bit-fields or
  overrides; the hand tests and `tests/check` must carry them. 8 leaves have
  `ck == 0` (not ICE, not ADDR): **to identify** before T3b.
- The gcc.dg initializer files are not on this machine; `tests/check` is the
  initializer test source.
- **Decision: the hook goes ahead behind a checker flag; the first T3b task
  is the five hook points plus the clear event, verified by `tast_data.py`
  on `tests/check`.**

### Verify list, closed (2026-10-18)

- `__builtin_constant_p` is always `K_ICE` (`ccall.c:2001`), never reaches the
  pass as a call.
- `bt_find`'s `pre` flag: the pass only takes names that start with
  `__builtin_`, so `pre` is true; closed with T3a.
- Implicit-declaration calls: libuv's `setproctitle` is reported "undeclared"
  with `sym == SYM_NONE`; stays unsupported (one unit).
- gcc keeps unused statics at `-O0` (`su`, `sc`, `sl.0` all in `nm` output),
  so the data oracle can compile the test files as they are.
- gcc accepts `long p = (long)&a;` at file scope and emits a 64-bit
  relocation, so the constness check must allow `cast(addr sym)` to an
  8-byte integer.
- String bytes are not retained, but one decoder exists: `lit_char_one`
  (`lit.c`) behind `lit_str_units`, and `fmt_decode` (`cformat.c`) builds the
  unescaped bytes of a narrow literal. T3b/T3c turn that into one
  width-aware `lit_str_bytes` and make `fmt_decode` use it, rather than a
  third decoder.
- Still to read when the work reaches it: whether a function-local `static`
  has an index declaration; `{[0]=f(), [0]=g()}` evaluation in gcc for
  automatic objects; `ck == 0` leaves.

### T3b, built (2026-10-18)

Initializers, static data, static and extern locals. The reasons "global
initializer", "static or extern local" and "braced initializer" are gone.

- **Recording (option 1 of 3.2).** The checker's initializer cursor appends
  `TIRec` elements to `Checker.tinit` when `--dump-tast` is on (`cinit.c`:
  `tin_*` helpers, hooks in `out_elem`, `push_level`, `pop_level`,
  `finalize_root`). `TI_CLEAR` with a `fence` covers union override and a
  fresh sub-object; `TI_BAD` marks flexible-array content. Off, it is one
  flag test per element (the callgrind gate was not rerun).
- **Pass side (`tast.c`).** `init_elems` drops contained extents (in bits),
  sorts a static object by offset and keeps an automatic one in source
  order, and emits `elem +byte [bit b width w] <value>` nodes. `data_node`
  makes `data sym elems..`; an automatic aggregate or array becomes `local`
  with elem kids (the rest is zero). A unit of initialized globals prints
  `unit .. data` with a root `block` of `data` nodes; `extern` locals make no
  node. `str` carries its code units (`str uN <hex>`), decoded by the one
  `lit_str_bytes`.
- **Verifier.** `elem`/`data`/`local` kids, bounds, static order and
  disjointness, and `sconst`/`slv`: every static value is built from
  `const`, `addr` of a static or global lvalue, `str`, `ptradd`, `cast`.
- **Data oracle** `tests/tast_data.py` (wired into `tests/run.sh`): evaluates
  every `data` node to bytes plus relocations and compares with
  `gcc-13 -O0 -c`. `tests/tast/init.c` and `data.c`: 55 objects, 0 failures.
  Corpus (env `TAST_FLAGS`): lua54 101 objects, 0 failures (2 skipped:
  typedef-named element types); libuv 13 compared (the rest are other
  operating systems' files that gcc cannot compile), 0 failures. Same-named
  function statics (`mode`) match in any order, since gcc numbers them
  `.0 .1` in an order that is not the source order.
- **Coverage** (`2026-10-18-tast-t3b-coverage`): lua54 unsupported 301 -> 206
  (floating 190, variadic/unprototyped definition 10, compound lvalue 2,
  `va_arg` 2, address-of-label 2 newly visible); libuv 150 -> 128
  (`STMT_EXPR` 111, floating 12). 69 + 15 data units, 0 verifier findings.
  Suite 2243 passed.
- **Open verifies, closed.** A function-local static has an index
  declaration (`--dump-symbols --verify-tast` on `init.c`/`data.c`: no
  findings). `{[0]=f(), [0]=g()}` in an automatic object is unsupported by
  decision (parked); gcc's evaluation was not needed.
- **`ck == 0` leaves** are `K_NONE`: the checker's "not constant" mark for an
  automatic initializer value. They are ordinary expressions.
- **Found, not ours:** the checker rejects `long big = (long)&ext + 8;`,
  which gcc accepts (parked).

### T3c, built (2026-10-18)

`stmtexpr` and `__func__`.

- `stmtexpr` is a value op: the block's statements, then (unless its type is
  void) the last expression statement's value converted to the expression's
  type. The value comes from the checker's `stmt_expr_value`; a labeled last
  statement is unsupported ("statement expression value form"). Declarations
  inside use the ordinary `decl`. Verifier: statements first, then one value
  of the node's type (void: statements only).
- `__func__`, `__FUNCTION__`, `__PRETTY_FUNCTION__` become a `str` of the
  enclosing function's name (`El.fname`); outside a function they stay
  unsupported.
- Test: `tests/tast/stmtexpr.c` (glibc-style `assert`, nested, value
  conversion, jumps and labels inside, empty).
- Coverage (`2026-10-18-tast-t3c-coverage`): libuv unsupported 128 -> 34,
  1,514 functions elaborate; lua54 unchanged at 206. Left in libuv: floating
  12, `__atomic_*` calls 15 (generic builtins, refused by `builtin_ft`),
  array type 2, one each of definition, `asm`, `va_arg`, lvalue form,
  `setproctitle`. Suite 2247 passed, 0 verifier findings.

### T3c decision

Keep `stmtexpr` in T3 (pull-forward stands): it is 102 of libuv's 150
remaining units, independent of the initializer hook, and needs only the
`str` bytes and the `__func__` string. Its order relative to T3b is free; do
T3c first if the hook proves harder than the spike suggests.

### T3 gate and leftovers (2026-10-18, run `2026-10-18-t3-gate`, commit `bad4a66`)

Globals ok; run and san 2250 passed, san findings 0; verify differ 2/1/0;
callgrind 3,895,360,854 (+0.03% over T2's 3,894,075,059). Coverage: lua54
206 unsupported (floating 190, variadic/unprototyped definition 10, compound
lvalue 2, `va_arg` 2, address-of-label 2); libuv 20 (floating 12, array type
2, one each definition, `asm`, lvalue form, `va_arg`, `setproctitle`,
`__atomic_fetch_add`), 0 verifier findings. The 8 units `posix-poll.c` skips
for errors fail identically under gcc (platform file): not a checker bug.
Leftovers are in `PARKED.md` ("TAST T3 leftovers").
