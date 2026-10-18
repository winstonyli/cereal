---
status: done
---

# TAST stage T2: plan

Implements stage T2 of `docs/TAST_DESIGN.md` (section 5) and nothing else.
Builds on the T1 pass (`src/c/tast.c`, `docs/TAST_T1_PLAN.md`). Checker facts
are from the tree on 2026-10-18; items not yet checked are marked **verify**.
Not started: needs a go-ahead.

## 1. Scope

In: pointer types (object, `void`, function pointers), arrays (decay, `[]`),
unary `&` and `*`, pointer arithmetic (`p + i`, `i + p`, `p - i`, `p - q`,
`p++`, `p += i`), pointer comparison and null-pointer-constant conversion,
pointer casts (pointer to pointer, pointer to and from integer), function
designators and calls through a pointer, `.` and `->`, structs and unions
(members, assignment by value, pass and return by value), bit-fields (load,
store, truncation), string literals as values (not as array initializers),
`T *p = e;` and `T a[N];` locals without initializer or with a scalar/pointer
initializer.

Out (the whole unit is `unsupported`, reason counted): braced and string
initializers of aggregates (T3), static data, floating point (T4), VLAs,
compound literals, statement expressions (T5), `member of rvalue`
(`f().x`, a value member needs a `getfield` value op, parked to T5),
`__builtin_*` calls and anything else T1 already rejects.

Result: pointers were 2,100 of 2,243 reasons on lua54 and 1,476 of 1,542 on
libuv (T1 results, design section 14), so T2 is where the coverage number
starts to mean something; global initializers (69 and 15) and floating point
(28) remain the next walls.

## 2. Design decisions

The aim is the T1 core, extended by a few ops, with every surface form a
sugar over them (design section 4, "small core"). No second path.

**Lvalues become a closed set of address-denoting nodes.** In T1 an lvalue is
a `sym` node and `load`/`store` take it. T2 keeps that and adds two lvalue
nodes, so `load`, `store`, `addr` and `field` are the only consumers:

| Node | Kids | Meaning |
|---|---|---|
| `sym` | none | the object (T1) |
| `deref` | pointer value | the object a pointer designates (`*p`) |
| `field` | lvalue | member at byte offset `val`; a bit-field also carries `bit` and `width` |

`a[i]` is `deref(ptradd(decay a, i))`, `p->f` is `field(deref p)`, `a.f` is
`field(lvalue a)`; nothing else is added for them.

**Value nodes added.**

| Node | Kids | Meaning |
|---|---|---|
| `addr` | lvalue | pointer to the object; `ty` is the pointer type. Array decay is `addr` of the array lvalue with `ty` pointer-to-element; a function designator is `addr` of the function `sym` |
| `ptradd` | pointer, integer | pointer plus index; `val` is the element size in bytes (the integer is first converted to `ptrdiff_t` by an explicit cast node); `p - i` is `ptradd` with a negated index |
| `ptrdiff` | pointer, pointer | difference in elements (`val` = element size), result `ptrdiff_t` |

`ptradd` and `ptrdiff` are the only new shape lowerings; both are sugar that
T6 expands into `cast`, `binop *` and `binop +/-` (the template table of
design section 4). Pointer comparison reuses `cmp`; `&&`, `||`, `!` and
conditions take pointers directly, as T1 leaves integer conditions raw (no
truth-value node).

**Conversions with pointers** reuse `cast` with the checker's own decisions:
null-pointer constant to pointer (`const 0` of the pointer type, read from
`EF_NPC`), pointer to pointer, pointer to integer, integer to pointer,
array and function decay (never a `cast`: `addr`). The required type comes
from the checker (`cexpr_rvalue_type()` gives the decay type already; the
assignment and argument compatibility conversions are the T1 `conv()` calls
with a pointer target), not a new rule table.

**Aggregates by value.** `load` and `store` accept struct and union types
(an aggregate copy); a `call` with an aggregate argument or result carries
the aggregate type; `return s;` likewise. Layout is not decided here: size
and offsets are read from `Record`/`Field` (design section 4), and the ABI is
a back-end matter.

**Bit-fields.** `field` of a bit-field carries `val` = byte offset of the
containing unit, `bit` = bit offset within it, `width`; the dump prints
`field #l +off bit b width w type`. A load reads and extends by the field's
declared signedness; a store truncates to `width`, and the value of the
assignment expression is the truncated value (C11 6.5.16). The checker marks
promotion with `EF_BFPROMOTE` (width below int): the elaborated `load` has
the field's declared type, and the promotion to `int` is an explicit `cast`
node via the same `promoted()` T1 already calls (it returns `int` on
`EF_BFPROMOTE`, round 1 below). `Field` stores
`off_bits` and `width`, so `val = off_bits / 8` aligned down to the declared
type's alignment and `bit = off_bits - val*8` is a derivation, not a new
fact (**verify** against gcc layout in the layout test below).

**Symbol identity.** The open question from design section 13: TAST `sym`
should carry the symbol index's `cindex_key`. That function takes a `CIndex`
declaration index (`csymidx.c:2469`), which exists only with `Checker.sx`,
and keys on anchor events. T2 therefore does not change `sym`; it adds a
measured check instead (task 9): with `Checker.sx` on, does every `sym` ref
that elaborates map to an index declaration? If yes, the key is a pure
addition in a later stage; if not, the identity story needs a decision
before T3. Left open on purpose.

## 3. Tasks, in order

Each ends with the build clean, `sh tests/run.sh` green and the T1 goldens
unchanged except where a task says so.

1. **Type gate.** Replace `ok_int()`/`why_type()` use for variables and
   expressions with a scalar-or-aggregate predicate: integer, enum, `_Bool`,
   pointer (to anything but a VLA), struct, union, array of those. Floating,
   `_Complex`, vector, `__int128` stay out. Existing T1 goldens do not move.
2. **Lvalue forms.** `lv()` accepts `deref`, `field`, `index` and the
   parenthesized forms by the table above; `ex()` of an lvalue wraps `load`
   (rvalue), except where the operand is the operand of `&`, `sizeof`, or an
   array or function (decay). One function owns "this operand is an lvalue
   used as a value": reuse the checker's `EF_LVALUE` plus the node's type.
3. **Decay, `&`, `*`.** `addr` nodes; `*p` of a function pointer yields the
   function designator again (decays back); `&*p` and `*&x` are not
   simplified.
4. **Pointer arithmetic and comparison.** `ptradd`, `ptrdiff`, pointer `++`,
   `--`, `+=`, `-=`, comparison and equality (including the null constant
   and `void *` on one side). Reuse the T1 `incdec`/`assign_op` shape with the
   operation type `ptradd`; the result type is checked against `c->ty[i]`
   with `agree()`. GNU arithmetic on `void *` and function pointers uses
   element size 1 (the checker already accepts it). A pointer converted to
   `_Bool` (`_Bool b = p;`) is a `cast` to `_Bool` (T6 lowers it to `cmp ne`).
   **Naming an lvalue twice.** T1's compound assignment and `++`/`--` name the
   lvalue twice, which was safe because lvalues were plain variables. With
   `deref`/`field` it duplicates side effects (`a[i++] += 1`, `*f() += 1`,
   `p[g()]++`). Rule: if any subexpression of the lvalue has `EF_SIDE`
   (volatile reads included), the unit is `unsupported: side effect in
   compound lvalue`, counted; temporaries arrive with the T6 templates. An
   `EF_SIDE`-free lvalue is evaluated twice, which is observably the same.
   The same rule covers `post` (value of `x++`) on such lvalues.
5. **Members and aggregates.** `field`, `.`/`->`, struct and union
   assignment, argument and return by value, `sizeof`/`alignof` already
   folded. Bit-fields as specified above.
6. **Calls through pointers.** Replace T1's `indirect call` rejection: the
   callee is a function pointer value (or a decayed designator), the
   parameter list comes from the pointee function type; the direct call stays
   the T1 form (a `call` holding the symbol) so T1 goldens are unchanged.
7. **String literals.** `EF_STRING` expression used as a value decays to an
   `addr` of a new `str` lvalue node holding the string id and length
   (including the terminator, wide strings by element width). A string as an
   array initializer stays out (T3).
8. **Dump, verifier, tests.** Dump lines for the new ops; the verifier
   gains: `deref`/`field`/`sym` are the only lvalues, `load` and `store`
   target lvalues only, `addr` takes an lvalue, `ptradd` kids are pointer
   then `ptrdiff_t` integer, `field` offset plus width fit the record size
   from `Record`, bit-field `bit + width` fits the declared type, an `addr`
   result type is pointer to the lvalue's type (or to the element, for
   decay). Tests in `tests/tast/`: `ptr.c` (arithmetic, difference,
   comparison, null, `void *`, `p++` on `char *` and `int *`), `array.c`
   (decay, 2-D arrays, `a[i]` with a `char` index and a negative constant,
   `i[a]`), `agg.c` (struct copy, nested member, union, by-value call and
   return, `->` on a nested pointer), `bitfield.c` (signed, unsigned, `_Bool`,
   width 1, width equal to the type, narrower than int, `++`, `+=`,
   assignment result), `fnptr.c` (call through a pointer, array of function
   pointers, `*fp`, `(*fp)(x)`), `str.c` (string used as `char *`, `sizeof`
   of a string, wide string), `unsup.c` additions (`f().x`, compound
   literal, VLA, floating member). Run in `tests/run.sh` group `13c` with the
   plain and forced-parallel flags, as T1.
9. **Layout regression and gcc differential.** `tests/tast/layout.c`:
   `_Static_assert(offsetof(...) == N)` and `sizeof` checks compiled by both
   `gcc-13` and cereal (must agree), plus the dump's `field` offsets against
   the same numbers; this is the "layout regression" of design section 5.
   Extend `tests/tast_gcc.py` to the pointer ops: compare the multiset of
   `POINTER_PLUS_EXPR`, `MEM_REF`/indirection and cast targets per function
   (lead generator, as in T1). Also the `cindex_key` check from section 2.
10. **Measure and record.** On lua54 and libuv: functions that elaborate
    (expected to grow from 30 and 21), the new reason histogram, verifier
    findings (must be 0), pass cost with the option on, callgrind with it off
    (within 1%). Raw output through `run.ps1`, a `docs/runs.md` row, the
    finding in the design doc, T1's coverage run as the baseline.

## 4. Exit checks

- Tasks' tests green; gate at baseline with the option off (tests, verify
  2/1/0, san 0, callgrind within 1%).
- `--verify-tast` finds nothing on the T2 tests and on every unit that
  elaborates in lua54 and libuv, and the count of those units is reported
  (the T1 check was close to vacuous: 30 and 21 functions).
- `layout.c` agrees between gcc-13 and cereal; the gcc differential runs
  and its mismatches are explained or listed.
- The measured numbers are in `docs/runs.md` and the design doc.

## 5. Risks and stop rules

- `promoted()` and bit-fields: if the checker's `EF_BFPROMOTE` is the only
  carrier of the width rule and it is cleared by a parenthesis or a cast,
  elaborate the promotion from the field's width directly and add a test.
  If both disagree anywhere, stop and record it in the design doc.
- Member offsets with `__attribute__((packed))`, `aligned`, zero-width and
  unnamed bit-fields, flexible array members and `FF_PACKED` fields: the
  layout comes from `Field`, but the derived `val`/`bit` split is unverified
  there. Test each; if the split cannot be derived uniformly, store
  `off_bits` in the node instead and let the consumer split it.
- Array types in parameters (`int a[]`) are already pointers in the
  declared type; confirm `csym()->ty` shows the adjusted type before
  trusting it (**verify**).
- Cost: each new case is a branch inside `ex()`; the pass is off the hot
  path, so the callgrind gate with the option off is the only budget.
- If a form needs per-tag code beyond `deref`, `field`, `addr`, `ptradd`,
  `ptrdiff`, `str`, stop and update design section 4 before adding it.

## 6. Review log

Round 1 (claims against the tree): `promoted()` (`cexpr_int.h:225`) returns
`int` for any node with `EF_BFPROMOTE`, which `e_member` sets only for a
bit-field narrower than int (`ccall.c:2320`), so the bit-field promotion
is the checker's own call, no new rule; the "verify" mark is resolved. The
paren/cast propagation of the flag is still unchecked, so the risk entry
stays. Parameter array adjustment exists as `type_param_adjust`
(`type.c:657`); that csym types are already adjusted stays a task-time check.
`cindex_key` needs a `CIndex` decl, confirming it cannot be attached to
`sym` without `Checker.sx`; the open question stays open, as written.
Round 2 (contradictions, gaps): compound assignment and `++`/`--` name the
lvalue twice, which T1 allowed only because lvalues were variables; with
`deref`/`field` that duplicates side effects, so a rule and an
`unsupported` reason were added (task 4). `void *` and function-pointer
arithmetic (GNU) had no element size, now 1. Pointer to `_Bool` had no
node, now a `cast`. Conditions on pointers: T1 leaves integer conditions raw,
so pointers are raw too (stated in section 2). Round 3: no further
material finding on a full pass of scope, tasks, exit checks and risks.
Residual risk the plan cannot remove: the gcc differential has no oracle for
bit-field loads (gcc prints `BIT_FIELD_REF` only after gimplification), so
bit-field correctness rests on the layout test and hand-checked goldens.

## 7. Progress

Tasks 1 to 8, the layout half of 9 and task 10 done, gate green (callgrind +0.21%)
(2026-10-18); results in TAST_DESIGN section 15. Suite 2233 passed. Deviations:

- `field` nodes hold byte offset, bit offset within that byte and width
  (the risk entry's fallback, derived from `find_field`'s total bit offset),
  not a declared-type-aligned unit offset. The layout test passes on 26
  members against gcc-13.
- Pointer-pointer comparison casts the right operand to the left's type when
  they differ (and an integer null constant to the pointer type); the `void *`
  case in `ptrcmp` shows a `cast`.
- `str` holds only its array type (bytes parked, PARKED.md).
- The `cindex_key` measurement (section 2) was not done: whether every
  elaborated `sym` maps to an index declaration is still open and is parked
  with the T2 leftovers.
- The gcc differential extension to pointer arithmetic counts was not done
  (parked).
- Locals of array type with any initializer elaborate as `unsupported:
  braced initializer` (T3 owns initializers).
