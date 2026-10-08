# The checker (P2a): declarations, types, constant expressions

Source: `src/c/check.c` (walk, scopes, end of unit), `cdecl.c` (declarations; attributes are `cattr.c`;
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

## Summaries and read sets (P2e, for P3)

Source: `csum.c`, `csum.h`.  Off unless `CheckOptions.summaries`
(`--summaries`), `dump_summaries` (`--dump-summaries`) or
`validate_summaries` is set; then `checker_unit` leaves a `UnitSummary` that
`checker_summary(c)` returns (valid until the next unit).

**Summary** of a unit: the file-scope entities it declares, redeclares or
defines, sorted by (namespace, name): ordinary identifiers (`ord`),
tags (`tag`), the external-linkage view of a name (`ext`: block-scope
`extern`s and implicit function declarations made by bodies; folded into
`ord` when it is the same symbol) and the `#pragma pack` state (`state`).
Per entry: kind, linkage, storage class as written, type digest, flags
(defined, tentative, `decl_external`, inline, thread, noreturn, weak,
implicit, error, proto/K&R definition, const-init, register-named,
complete), the value of an enumerator, the layout digest of a tag, and two
entity digests: `iface` (what readers see: everything but the
definition-present bits) and `full`.  `UnitSummary.digest` folds all
entries; `sig` folds all but implicit declarations, so a body-only edit of a
function leaves the declaration entry, `digest` and `sig` alone unless the
body adds or removes an implicit declaration.  Names are hashed by spelling,
never by interner id.

**Type digests** are 64-bit, structural and memoized per type-table entry
(qualifiers mixed in afterwards): builtin kind; pointer/array (count,
incompleteness)/VLA/function (return, parameters, variadic, unprototyped)/
vector/complex over their parts; typedef by name, alignment and target.
A struct, union or enum is its identity only: the digest of (unit key, tag
spelling or none, ordinal among the unit's records with that spelling, kind).
The unit key defaults to a digest of the names the unit declares (so editing
a body or a member list keeps the tag's identity; `checker_set_unit_key`
lets P3 supply its own); a unit that declares no name uses a hash of its
tokens.  Completing a tag in a later unit does not change its identity.
**Layout** is a separate query: complete flag, size, alignment, flags and
every member's name, type digest, offset, width and alignment (enums:
underlying type and completeness).

**Read set**: per distinct name, one read with the digest of what the name
denoted when the unit looked it up at file scope.
- `clookup` reports a lookup that resolved to a file-scope binding or to
  none (a miss counts, digest 0).  A de-duplicating stamp per identifier
  makes this one inline load on repeat lookups; block-scope bindings are
  not reads.  `name` reads see the `iface` digest (a tag: which type it is).
- `full` reads are taken before the unit declares, redeclares or defines a
  name (`cbind` at file scope, `pushdecl`, a tag definition,
  implicit declarations): the name's state on entry, including whether it
  was defined.  Afterwards reads of that name are the unit's own and are
  not recorded.
- `layout` reads come from `type.c`: the accessors of a record's or enum's
  contents (`type_record`, `type_enum`, size, alignment, completeness,
  underlying type) report to the hook for records older than the unit;
  keyed by the record's identity digest since the record may have no name
  (`typedef struct {..} T`).  This is the implicit incomplete-to-complete
  and `sizeof`/member-access dependency; naming a tag in a pointer type is
  only a `name` read.
- `ext` reads are the external-linkage table lookups (block-scope
  `extern`, K&R prototype checks).  `state pack`: a struct completion used
  `#pragma pack`.  Reads are sorted by (namespace, mode, name, key).

`summary_valid(summary, lookup, ctx, &bad)` is true when every read yields
the same digest from `lookup(ctx, ns, name, key, mode)` (0: absent);
`checker_file_digest` is such a lookup over a checker's current file-scope
state, so a unit's read set can be validated against any other state at
its entry.  Units are validated on entry state: the unit's own first
declaration of a name is a `full` read of "absent".

`--dump-summaries` prints each unit (`unit N key= digest= sig=`, entries,
`read ns mode key digest name` lines; deterministic, digests are
target dependent).  `--validate-summaries=FILE` takes such a dump and, at
the entry of unit N, prints `validate unit N: valid|invalid (ns mode name)`
for FILE's unit N against the state built from the new input.
`summary_write` / `summary_load` are the serialization (read sets and unit
digests are reloaded, entries are not).  Tests: `tests/summary/` goldens and
`tests/summary.py` (context independence, body edits, reordering, validity).

What P3 must know: units are validated by entry state, so a unit's reads are
only as good as the *order* of earlier units; diagnostics of a unit also
depend on checker-global state that is not in the read set (the
once-per-TU `undeclared` note, headers already suggested, `#pragma pack`
stack beyond its digest, the parser lookahead at unit boundaries) and on
`c->quiet`; block-scope symbols and unit-local types are not summarized.
Cost on `uvloop/loop.c` (11.3k units, 58k reads): about +1.5% instructions
on parse+check, +4% on the check pass alone.

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
- typedef notes follow gcc's aka rule (omitted for same-name tags, anonymous
  structs and system-header typedefs); rarer spellings may still differ.
- C2X extensions accepted under `-std=c99` with pedwarns: `[[attr]]`, decl after
  label, storage class in compound literals, enum underlying types, `0b`
  constants, decimal-float constants.  `-Wundef` is not part of `-Wall`/`-Wextra`.
- gcc.dg parity (`-fsyntax-only -std=c99 -pedantic`): residuals are attribute
  argument-count checks, `__builtin_*` argument checks, some pedantic
  'before C99' wording and constant-folding of initializer elements.
- Parse-level message wording: gcc's "expected '=', ',', ';', 'asm' or
  '__attribute__' before ..." variants differ in a few declarator error
  cases (the parser's, not the checker's).
- Expression and conversion checks (`cexpr.c`): assignment/argument/return
  conversions with notes, calls, implicit declarations and built-ins (the
  prototype table in `cbuiltin_tab.h` covers the common libc/`__builtin_`
  set only), lvalue/read-only checks, operand errors, casts, and the
  `-Wparentheses`, `-Wunused-value`, `-Waddress`, `-Wbool-compare`,
  `-Wtautological-compare`, `-Wtype-limits`, `-Wsign-compare`,
  `-Wsizeof-array-argument` and `-Woverflow` families.  Known diffs: a
  parameter's top-level `const` is missing from the `{aka ...}` of a
  function-pointer typedef in call-argument notes; `-Wtype-limits` and
  `-Wtautological-compare` do not model every gcc folding (e.g. through
  `fold_build` rewrites of nested constant expressions); `-Wunused-value`
  columns for some folded comparisons follow `EF_GCCFOLD`.
- Jump-into-VLA-scope and attribute argument counts are not implemented.
- Initializers (`cinit.c`): gcc 13's digest_init / process_init_element /
  push_init_level model (designators including ranges, brace elision, string
  initializers, array completion, flexible-array and VLA rules, compound
  literals, constant-initializer rules, `-Wmissing-braces`,
  `-Wmissing-field-initializers`, `-Woverride-init`, pedantic variants).
  Known diffs: `g == g` (same address) is not folded to 1, so it is reported
  as non-constant; `-Waddress` "will always evaluate as 'true'" is not emitted
  for bool initializers; `int f(void) = {1};` (and typedef/parameter
  initializers) lack the extra "invalid initializer" note; bit-field
  conversion warnings from pointer-to-int use `unsigned int` rather than gcc's
  `unsigned char:3` spelling, float-to-bit-field overflow and assignments to
  bit-fields (`b.a = 9`) are not warned; the location of "overflow in constant
  expression" for elements emitted from the pending set (random-order
  designators) differs from gcc's stale input_location; an error inside an
  erroneous scalar brace group followed by a compound literal may add one extra
  diagnostic; `-Wdesignated-init`/traditional warnings, the c89-only
  require_constant_elements pedwarn and the maybe_const pedantic warning are
  skipped; very large range designators (`[0 ... 1000000]`) cost memory
  proportional to the range.
