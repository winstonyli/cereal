# The checker (P2a): declarations, types, constant expressions

Source: `src/c/check.c` (walk, scopes, end of unit), `cdecl.c` (declarations;
a port of gcc 13's `c-decl.cc` and the parts of `c-parser.cc` that decide
where diagnostics point), `cexpr.c` (expressions, constant folding),
`type.c` (hash-consed types, layout), `check_int.h` (internals).  Run it
with `cereal -fsyntax-only` (or `cereal check`).  The oracle is gcc 13.3 at
`-std=c99 -pedantic`; see `docs/PARSER.md` (Type checking) for the plan.

## The walk

The parser's tree is a post-order node array; node `i`'s subtree is
`cfirst(i) .. i` (`cfirst(i) = i + 1 - size`).  `node_children` (ast.c)
lists the direct children by hopping over subtrees.  `visit()` in check.c
goes through the nodes of a unit in index order and sets `Checker.cur_node`;
declaration tags go to `cdecl_node`, expression tags to `cexpr_node`.
Marker nodes (declarator end, scope open/close, function-body start) give
the checker the points where post-order is too late (`int x = sizeof x;`).
A unit with syntax errors is checked `quiet`: state is built, nothing is
reported.

## Per-node arrays

One entry per node, sized to the unit: `ty` (TypeId), `cv` (constant value
or tag-specific payload), `cb` (symbol ref + 1 of an address base, or
`CB_NODE|node`), `ck` (constant kind: `K_NONE/ERR/ICE/FOLD/FLOAT/ADDR`),
`ef` (expression flags: lvalue, bit-field, null pointer constant, ...), and
`par`.  The per-tag use in declaration nodes is listed at the top of
cdecl.c.  `ef` is reused per tag, so a consumer must reset entries it reads
(a stale `ef` once produced false `[*]` errors).

## Scopes and symbols

One `CSym` per entity.  Symbols with external linkage and block-scope
declarations of externals are persistent (`gsyms`, shared across units);
the rest live for the unit (`lsyms`).  A `SymRef` indexes one or the
other (`SYM_LOCAL` bit).  Bindings of both namespaces (ordinary and tags)
go into one log (`BindVec`); `top[ns][ident]` is the innermost binding and
scope marks give O(1) scope exit.  Prototype scopes are saved so tags
declared there are reported like gcc.  `pushdecl` and `duplicate_decls`
are ports of gcc's (conflicting types, linkage, merged qualifiers,
"previous declaration" notes).  `b->nested` semantics for block-scope
externs are kept.

## Specifiers

`SPECS` nodes build a `Spec` (gcc's `c_declspecs`: type-specifier word
counts, storage class, qualifiers, `_Noreturn`, `inline`, alignment,
attributes) and push it on `Checker.specs`.  The consumer (DECL,
FUNC_DEF, PARAM, TYPE_NAME, MEMBER_DECL) pops it; `pop_specs` keeps the
stack balanced even after errors.  Conflicts are diagnosed word by word as
gcc does ("two or more data types", "duplicate 'const'", ...).

## Declarator resolution

`grok` is `grokdeclarator`: the declarator's chain (pointer, array,
function, parenthesized) is applied inside out to the specifier type.  It
enforces array rules (incomplete or function element types, negative or
non-constant sizes, VLAs: an array of a variably modified type is a VLA),
function rules (returning arrays/functions, `void` parameters, K&R
identifier lists), bit-field rules, alignment (max 2^28) and storage-class
legality per context (`DC_NORMAL/FIELD/PARM/TYPENAME`).  Records use
`start_struct`/`finish_struct` (layout through `type.c` and `target.c`),
enums `start_enum`/`finish_enum` (an enumerator has the type of its value
until the enum is finished).  `INIT_DECL` completes array types from
initializers; tentative definitions are resolved at the end of the unit.
`__auto_type` takes the initializer's type.

## Diagnostics policy

Messages, options and locations follow gcc 13: errors, pedwarns (warning,
or error under `-pedantic-errors`, enabled by `-Wpedantic` unless the node
is inside `__extension__`, via `vped`), and warnings with their `-W`
names.  Locations are gcc's: the spelling location for macro tokens, display
columns (tabs expand to the next multiple of 8) in the `file:line:col:`
header.  Diagnostics are buffered in creation order and flushed in that
order, so sequences match gcc's even where gcc reports in an odd order.
Some diagnostics name the parser's lookahead token (`cd_ltok`, `iloc`)
because gcc's `input_location` is where the parser stopped.  Once an error
type is produced, dependent checks stay silent.

## --dump-types

One line per declaration, in order: `typedef NAME = TYPE`,
`var NAME: TYPE [static|extern|tentative]`,
`func NAME: TYPE [static|extern|inline|defined]`,
`enumconst NAME = VALUE (TYPE)`, and record layouts when a struct or union
is completed.  Block-scope names are printed as `func:name`.

## Testing

`tests/check/X.c` with `X.expected` (stderr then stdout, from cereal, after
checking the diagnostic headers equal gcc's).  An optional first-line
`// flags: ...` adds options.  `run.sh` runs each case plainly and with
`--cells -fparallel-chunk=1 -fparallel-threads=2`.  Corpus and gcc.dg
comparisons use gcc 13.3 with `LC_ALL=C`.

## Gaps (known differences from gcc)

- `restrict` misuse in declaration specifiers: gcc reports the error twice,
  cereal once.
- `enum S;` for an existing struct tag lacks the extra "empty declaration
  of 'enum' type does not redeclare tag" pedwarn.
- "'return' with a value, in function returning void" is reported by gcc in
  a few cases with an incomplete struct return; not modelled.
- `empty enum is invalid` is not reported.
- typedef notes print `'S'` with an extra `{aka 'struct S'}` where gcc omits it.
- Binary constants (`0b101`) are rejected in `-std=c99`; gcc accepts them
  as an extension with a pedwarn.
- Parse-level message wording: gcc's "expected '=', ',', ';', 'asm' or
  '__attribute__' before ..." variants differ in a few declarator error
  cases (the parser's, not the checker's).
- Initializer checks (non-constant initializers, VLA initialization),
  jump-into-VLA-scope, attribute argument counts, call argument checks and
  statement-level checks are not (or not fully) implemented; gcc.dg shows
  these as accepts-invalid.
