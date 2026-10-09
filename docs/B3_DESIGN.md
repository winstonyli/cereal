# B3 design: typeDefinition, document symbols, signature help, semantic tokens and completion for C names

Status: designed 2026-10-09, before implementation (ROADMAP Track B,
B3). It builds on the C symbol index of B2 (B2_DESIGN.md; LSP.md, "C
symbol index"). Line numbers and file names refer to the WSL tree
`~/cereal-t`.

## 1. Goals

| Request | Today | After B3 |
|---|---|---|
| `textDocument/typeDefinition` | not advertised | the definition of the type of the C entity at the cursor (its typedef, struct, union or enum) |
| `textDocument/documentSymbol` | the file's `#define`s | plus the file's C declarations: functions, objects, typedefs, tags, with fields and enumerators as children |
| `textDocument/signatureHelp` | function-like macros | plus calls of C functions and of function pointers (objects, parameters, fields, typedef'd pointer types) whose declaration the index knows |
| `textDocument/semanticTokens/*` | `macro`, `parameter` (macro parameters); `declaration` | plus every C name the index records, by kind, with `declaration`, `readonly` and `static` |
| `textDocument/completion` | the macros visible at the cursor | plus the C names of the ordinary namespace visible there |

Macros keep precedence everywhere, as in B2: a name that the macro index
answers for strongly is a macro.

Not in B3: member completion after `.`/`->`, tag completion after
`struct`, keywords (section 9).

## 2. What exists (verified in the code)

- **Macro side.** completion (`index_visible`), document symbols
  (`pp->macros`), semantic tokens (`semantic_data`: macro refs and
  definitions as `macro`, macro parameters as `parameter`; legend
  modifiers `declaration` and `readonly`, the latter unused), signature
  help (`lsp_signature_help`: scans the *editor* text back from the
  cursor to the unmatched `(` and the name before it, counting top-level
  commas; answers for a function-like macro). All in
  `src/lsp/features.c`.
- **Index data** (`src/c/csymidx.h`). Events (file, offset, length, role
  DECL/DEF/REF, flags BODY/EXPANSION/ARG/SYSTEM) pointing at decls (name,
  hover text, kind, linkage, flags BUILTIN/IMPLICIT/SYSTEM/TENTATIVE,
  `scope`). `scope` is set only for block-scope names without linkage and
  labels, and is the extent of the *external declaration* (the checker
  unit) around them, not their block. Only the extents some decl refers
  to are kept.
- **Missing for B3:**
  1. the type of a decl as a decl (typeDefinition);
  2. a member's record or enum (document symbol children);
  3. block extents and a position-to-scope query (completion, signature
     help by name, "is this declaration at file level");
  4. the extent of every external declaration (document symbol ranges);
  5. `const` and `static` per decl (semantic token modifiers);
  6. parameter lists (signature help): see 5.3, read from the
     declaration's text rather than stored;
  7. system declarations the unit never uses: today a system decl enters
     the index only when user code uses it (B2 section 5.3), so
     completion would not offer `strlen` before its first use.
- **Mid-edit behaviour, measured** (`cereal check --dump-symbols` on
  broken text): `b = add(a,` followed by `}`, an unclosed `add(a,` at
  the end of the file, and `s.` followed by `}` all keep every
  declaration of the unit indexed (globals, the function, its
  parameters and locals, inner blocks), and the half-typed call still
  records its `add` and `a` uses. The parser's recovery stops at the
  statement, not the function.

## 3. Index additions

### 3.1 CIdxDecl

```c
uint32_t type;    /* 1 + the decl of its type, 0: none (3.3) */
uint32_t parent;  /* 1 + a field's record, an enumerator's enum; 0 */
```
and two flags: `CIDF_READONLY` (an object, parameter or field whose
canonical type, arrays stripped, is const-qualified: `const int a[3]`
and `CI x` with `typedef const int CI` count, `const char *s` does not;
every enumerator) and `CIDF_STATIC` (internal linkage, or declared
`static` at block scope). 16 to 24 bytes per decl.

### 3.2 Scopes: a tree of every external declaration and block

```c
typedef struct CIdxScope {   /* [begin, end) of file; sorted by (file, begin) */
    uint32_t begin, end;
    uint32_t file;
    uint32_t parent;         /* 1 + the enclosing scope; 0: a root */
} CIdxScope;
```
- **Roots** are the external declarations (the checker's units), as
  `csx_unit_begin` measures them today (presentation points of the first
  and last token). **Children** are the checker's own scopes: blocks,
  prototype scopes, a function body (`N_SCOPE` to `N_SCOPE_END`, hooked
  in `scope_open`/`scope_close`, check.c), with `end` just past the
  closing token. This is the checker's scoping recorded, not imitated.
- `CIdxDecl.scope` becomes the decl's **innermost** scope (still only for
  block-scope names without linkage). A definition's parameters move to
  the function body's scope in `csx_param_def`, so they are visible in
  the body; a prototype's stay in the prototype scope. Labels take the
  root (function scope).
- **Rename (A3.9)** used the extent of the external declaration; it now
  walks `parent` to the root, which is that same extent
  (`cindex_scope_root`). Unchanged behaviour.
- **Kept:** scopes whose begin and end lie in one user file that has
  events. A root that spans files is dropped with all its blocks (their
  decls get scope 0, the whole unit, as A5 says today); a block that
  spans files is dropped and its decls take the nearest kept ancestor.
  System-header scopes are dropped (no events are presented there).
- **Query** `cindex_scope_at(ix, file, off)`: binary search for the last
  scope starting at or before `off`, then walk `parent` to the first one
  whose end is past `off`; 0 if none. Correct because scopes nest
  properly: every scope containing `off` is an ancestor of the last one
  starting before it.

### 3.3 The type link

Computed where the hover text is (unit end for block-scope symbols,
`csx_finish` for the rest), from the checker's type, by one function
`type_decl(TypeId)`: strip pointers, arrays, VLAs, function types (to
the return type), vectors and complex; stop at the first typedef (its
decl, through a map from the typedef's `TY_TYPEDEF` entry, which is
unique per declaration, to its decl id, filled in `sym_id`) or record or
enum (its tag decl, looked up; never created for a named tag, so no decl
without events appears). Per kind:

| Decl | `type` |
|---|---|
| function | its return type |
| object, parameter, field | its type |
| typedef | the type it names (one step: `typedef S_t *P` gives `S_t`) |
| struct, union, enum | itself |
| enumerator | its enum (C types it `int`; the enum is what a reader means) |
| label | none |

**Anonymous records and enums** (`typedef struct { int x; } T;`,
`enum { A, B };`) get a **nameless decl** (no events), so that `T`'s type
and `x`'s parent have something to point at. It is created by
`type_decl`, by `csx_enum` and for records with named fields at finish.
Fields of an anonymous *member* (`struct S { union { int a; }; }`) take
the outermost record as parent (`S`, or the nameless decl when that is
anonymous too), as C makes them members of `S`.
Nameless decls have no events, so no position query reaches them;
`--verify-symbols` excuses them from "has a declaration event", and
`--dump-symbols` prints them as `<anonymous>`.

### 3.4 System declarations up front

At `csx_finish`, every file-scope symbol declared in a system header
that has no decl yet gets one, with the one DECL/DEF event at its
declaration that a first use gives it today (`sym_id(.., lazy)`). Only
completion and signature help by name reach them; references, hover and
rename are unchanged (an unused name has no other events). Cost: about
1,500 to 2,000 decls for a unit including the usual libc headers
(cereal's `src/main.c` has 2,804 `--dump-types` lines, most of them
system); measured in phase 1 of the implementation (section 8).

### 3.5 Builder changes (src/c/csymidx.c, check.c)

- `csx_scope(c, tok, open)` from `scope_open`/`scope_close`; a stack of
  open scopes; roots pushed by `csx_unit_begin`, everything still open
  closed at `csx_unit_end` (error recovery leaves scopes open).
- `BDecl.unit` becomes `BDecl.scope` (builder scope id); `type`,
  `parent` added.
- At finish: drop and remap scopes (3.2), sort them, map decls.

## 4. Shared queries (csymidx.c, used by the server and `cereal query`)

- `cindex_scope_at`, `cindex_scope_root` (3.2).
- `cindex_visible(ix, path, off, &out)`: the decls of the ordinary
  namespace (function, object, parameter, typedef, enumerator) visible
  at the position, one per name, sorted by name:
  - A decl is visible at P if one of its DECL/DEF events e (not in a
    stale file) **encloses** P and **precedes** it. Its scope S is the
    decl's `scope` if set, else `cindex_scope_at(e)` with a root counting
    as file level. S encloses P if it is file level or P lies in S's
    extent (same file). e precedes P if it is in another file (a header,
    taken as included before) or at or before P.
  - Shadowing: of two visible decls with one name, the one whose S is
    innermost wins (sorted scopes put inner ones at higher indices along
    one chain); file level loses to any block.
  - Nameless decls and decls with no DECL/DEF event (implicit functions,
    builtins with no location) are not offered.
  - Cost: one pass over the events with a binary search each, about
    O(events log scopes): milliseconds for the measured units (65k
    events for zstd.c).

## 5. Features

### 5.1 typeDefinition

The C entities at the cursor (`c_decls_at`, the B2 merge: a strong macro
answer gives null, macros having no type); their `type` decls; those
decls' DEF events, else DECL (`cindex_select(CIQ_DEF)`). Nothing for a
nameless type (no events) or a builtin type. `cereal query type
FILE:L:C` prints the same, like `def`.

### 5.2 documentSymbol

The macros as today, then the C declarations of the requested file:
- **Entries:** each DECL or DEF event in the file of a function, object,
  typedef, struct, union or enum decl with `scope` 0, not spelled in a
  `#define` body, and lying at file level (`cindex_scope_at` gives a root
  or nothing: this drops block-scope `extern`s and tags declared inside
  functions). A prototype and the definition are two entries (as clangd
  lists them).
- **Range:** the root (external declaration) containing the event,
  widened to contain the name (a root inside one macro invocation is
  `[invocation, +1)` while its name may be an argument after it); the
  name if no root contains it. **selectionRange:** the name. **kind:** a table by
  CIdxKind (Function 12, Variable 13, Class 5 for a typedef, Struct 23
  for struct and union, Enum 10, Field 8, EnumMember 22). **detail:** the
  first line of the hover text.
- **Children:** the members (decls whose `parent` is X) with their DEF
  event in this file, in offset order, under the entry whose `type` is X.
  A tag's type is itself, so `struct S {..}` holds its fields; `typedef
  struct {..} T` and `struct {..} v` hold the anonymous record's. When
  several entries have type X, the first DEF entry of X itself takes the
  children, else the first entry in the file. Enumerators whose enum no
  entry takes (`enum { A, B };`) are entries of their own when they lie
  at file level (the same test as above; a block's enum is left out).
  Fields that no entry takes are left out.
- Child range = selectionRange = the member's name.

### 5.3 signatureHelp

1. The existing scan of the editor text gives the callee name before the
   unmatched `(` and the count of top-level commas.
2. A function-like macro defined at that place answers (as today).
3. Else the C entity: the decls at the name's position (the event, which
   resolves fields, `p->cb(` and shadowing exactly); if none (the call
   is not in the index), the visible decl with that name (`cindex_visible`).
4. Else, as today, the macro of any version.
5. **The parameter list** is read from the declaration's text in the
   snapshot (as the macro help reads the editor's): the decl's DEF event,
   then its DECL events, each not spelled in a `#define` body and not
   pasted (a system declaration's single event is used), then, if none
   gives a list, the same for its `type` decl when that is a typedef
   (`callback_t cb;`), at most 4 steps. From the end of the name: skip
   white space and comments, balanced `[..]` and `)`, then a `(` must
   follow; the list runs to the matching `)`. White space (newlines,
   comments) collapses to one space. A `#` directive inside, or more than
   4 KiB, gives up. `(void)` is no parameter.
6. **Answer:** label `NAME(P1, P2, ...)` with each parameter as written
   (`int (*cmp)(const void *, const void *)`), parameter label offsets,
   `documentation` the hover text, `activeParameter` the comma count
   (capped at the last parameter when it is `...`). The macro and C
   answers share one writer.

What it covers: functions; objects, parameters and fields of
function-pointer type declared with a parameter list or through a
typedef. Not covered: calls through an expression (`(*fp)(`,
`tbl[i](`), a declaration whose name comes from `##`.

### 5.4 Semantic tokens

`semantic_data` adds, for the requested range, the C index's events in
the file (a binary search to the first event at or after the range's
start), except those spelled in a `#define` body (one body token can name
several entities), pasted ones (they sit on the macro name) and system
ones; none when the file is stale.
- **Types** (legend appended after `macro`, `parameter`, so macro
  tokens keep their indices): function, variable, parameter, type
  (typedef), enumMember, property (field), struct (struct and union),
  enum. Labels get no token (no standard type).
- **Modifiers:** declaration (DECL and DEF events), readonly
  (`CIDF_READONLY`), static (`CIDF_STATIC`); legend order declaration,
  readonly, static.
- Overlaps keep the existing rule (the first token after sorting wins).
  A C event and a macro token should not start at one place: expanded
  macro names are never C events, argument tokens are not macro refs,
  and body and pasted events are left out.
- The kept whole-file result is also keyed by whether the snapshot had
  its index (a snapshot's index is set once, NULL to non-NULL), so a
  result computed before the check published is not reused after. Not
  by the index pointer: freed memory can be reused for the next one.

### 5.5 Completion

The macros as today (`index_visible`), then `cindex_visible` at the
cursor, minus names that a visible macro hides; each item has the label,
a kind from the table (Function 3, Variable 6, Class 7 for a typedef,
EnumMember 20) and `detail` the hover text. Prefix filtering is left to
the client (`isIncomplete` false), as for macros.

## 6. Code that does not parse, and waiting

- **Which text.** Every B3 request answers from the newest snapshot's
  own index, not from an older one carried forward. The measurements in
  section 2 show why that is enough: an index built from half-typed text
  still has every declaration of the unit and even the call being typed.
  Positions in the request are the snapshot's, like every other request;
  signature help reads the editor text, which is the snapshot's once the
  wait below has succeeded.
- **D1 extended.** The wait of B2 decision D1 (up to 1.5 s while the
  newest edit has no snapshot or its check is pending) applies to every
  request that reads the C index: now also typeDefinition,
  documentSymbol, signatureHelp, completion and the three semantic token
  requests. One rule in the server: a column in the request table.
- **After a timeout** (a check over 1.5 s: units near the 4 MiB limit),
  the request answers from what is there: the macros alone if the newest
  snapshot has no index yet, as for the B2 requests. Semantic tokens then
  lack the C names until the next request: the client is not told to ask
  again (parked, section 9).
- **Header documents.** In a header that is a document of its
  includer's unit, every declaration in another file counts as before
  the position (4), so completion there also offers the main file's
  names declared after the `#include`. Accepted: positions in two files
  are not ordered by the index.
- **Stale snapshot.** If the newest edit's snapshot is still being built
  when the wait ends, the previous snapshot answers with its index;
  positions are then off by the edit, as for every other request.
- **No carry-over.** Keeping the previous index alive across edits (and
  mapping positions through the edit) was considered and rejected: it
  would retain a second snapshot per unit, and the newest index is
  already good on broken code.

## 7. Limits: the 4 MiB check limit and memory

- Units over the check limit (and headers opened on their own) have no
  index: every B3 feature then answers for macros only, as B2's do. No
  new limit.
- **Retained memory** grows by 8 bytes per decl (`type`, `parent`), 16
  per external declaration and block (scopes), and the eager system
  decls (3.4). The dense worst case of B2 (a 4 MiB struct of `int fN;`
  members, 29.3 MB) gains about 2.6 MB of decl fields and one root; a
  4 MiB file of `int vN;` declarations (23.3 MB, about 330k of them)
  gains 2.6 MB of decl fields and 5.3 MB of roots, about 31 MB in all,
  slightly over the ~30 MB budget of B2_DESIGN.md (phase 3, item 4).
  Accepted if measured so: roots are what gives document symbols their
  ranges. Measured in section 8.
- **Responses.** Completion lists every visible name, macros included
  (as today, where a libc unit already sends thousands of macros).
  Semantic tokens grow to one token per indexed name, bounded by the
  events (5 integers each).

## 8. Implementation order, tests and estimate

1. Index additions (3.1-3.5), `--dump-symbols` printing `type #N`,
   `parent #N`, `readonly`, `static`, the innermost `scope` lines and
   nameless decls; `--verify-symbols` checks that scopes are sorted and
   nested and that `type`/`parent` are in range. Goldens: `tests/symidx`.
   Measure memory (main.c, cexpr.c, zstd.c, the two dense files) and the
   eager system decls.
2. typeDefinition; `cereal query type`; `tests/query/csym.cmd` and
   `tests/lsp/csym_b3` (one session for all of B3).
3. documentSymbol, 4. signatureHelp, 5. semantic tokens, 6. completion
   (`cereal query visible` adds the C names).
- **Estimate:** about 550 lines: csymidx.c/h ~250 (scopes ~80, type and
  parent ~70, system decls ~10, visible and scope queries ~70, dump and
  verify ~20), check.c ~5, features.c ~230 (documentSymbol ~70,
  signature help ~80, semantic tokens ~40, completion ~25,
  typeDefinition ~15), server.c ~20, main.c ~30. Against ROADMAP's 350.

## 9. Parked

- **Member completion** after `.`/`->`: the data exists (`type` of the
  name before the dot, members by `parent`), but it is a different
  request context (the expression's type, not the scope); not B3.
- **Tag completion** after `struct`/`union`/`enum`, label completion
  after `goto`, keywords: the cursor context, not scope data.
- **`workspace/semanticTokens/refresh`** after a check that ended past
  the D1 wait: needs server-to-client requests, which the server does
  not send today.
- **Carrying the previous index** across edits (section 6).
- **Signature help through an expression** callee (`(*fp)(`, `a[i](`).
- **Blocks written inside a macro argument** (`WITH_LOCK(m, { int x;
  ... })`): scope extents are presentation points, so such a block is
  `[invocation, +1)` and completion inside it does not offer `x`. Using
  spellings where both braces are spelled outside `#define` bodies would
  fix it (about 15 lines); not needed for the common `FOREACH(..) {`
  form, whose braces are in the source.

## 10. Review rounds

Four rounds were run on this text before implementation, each checking
claims against the code in `~/cereal-t`.

1. **Claims against the code.** Found: the claim that macro tokens sort
   first at a tie with a C event (no such tie can occur; replaced by why);
   wrong memory figures for the `int vN;` file (5.3 MB of roots, about
   31 MB, over the budget; stated and accepted); `const int a[3]` is not
   const at the top of its canonical type (readonly now strips arrays);
   enumerators of a block-scope enum would have become file-level entries
   (now the same file-level test); the claimant rule for children named
   no order; an anonymous record inside an anonymous one had no parent.
   Confirmed: `TY_TYPEDEF` entries are made per declaration (`add_ent`,
   not hash-consed), so the typedef map is exact; a function body's
   `N_SCOPE` (NF_PARAMS) is open when `body_visit` calls
   `csx_param_def`, and `N_BODY` sits at the `{` inside it.
2. **Lifetimes, threads, other documents.** Found: keying the token
   cache by the index pointer can be fooled by reused memory (keyed by
   index presence instead); in a header document every other file's
   declarations count as earlier (documented); the B2 requests' wait
   (D1) had to become a property of the request, or each new request
   would need its own exception. Confirmed: all queries read the
   immutable index on the protocol thread; rename's two checks compare
   kinds, linkages, names and events, not the new fields, and both runs
   add the same system decls, so rename is unchanged (a new name that is
   an unused system function now gives a changed event decl instead of a
   new lazy decl: still refused).
3. **Edge cases of the new data.** Found: a root inside one macro
   invocation can end before its name, which LSP forbids for
   `selectionRange` (range widened); blocks written in a macro argument
   collapse to the invocation (parked with the fix). Walked: dropped
   scopes still nest (a subset of a nested family, parents remapped to
   the nearest kept ancestor); zero-length macro blocks; K&R identifier
   lists and implicit-int parameters (moved to the body by
   `csx_param_def`); nested functions; shadowing order along one chain;
   prototype parameters (visible only in their parentheses); a name
   declared both in a header and later in the main file (the header
   counts); signature help on `int (*fp)(int)`, `int (*fps[3])(int)`
   (callee `]`: null), `fn_t foo;` (through the typedef), a K&R
   definition, `PARAMS((..))` style (no list: next event or null).
4. **Consistency pass** over the revised text (legend indices kept for
   macro tokens, the stale-file rule in every feature, the estimate's
   parts, section 1 against sections 4 and 5): nothing material.

## 11. Results and corrections (Round 197)

Implemented as designed in sections 3 to 6; HISTORY Round 197 has the
details. Corrections found while building it:
- **Claimants of members** (5.2): an enumerator or field whose `type` is
  the record or enum must not claim its members. In `enum { OFF, ON };`
  the first enumerator has the nameless enum as its type and took `ON` as
  a child. Only entries (not members) claim now; `tests/lsp/csym_b3`
  covers it.
- **Visible macros against C names** (5.5): `index_visible` returns
  macros in definition order, not by name, so the C names are filtered by
  `macro_at_version(name, index_seq_at(at))` per name rather than by a
  merge.
- **`cereal query type`** prints `no type definition` for a builtin or
  nameless type, for a macro and for a position with no entity (it fell
  through to "unknown query" when no C entity was there). The query tests
  are `tests/query/b3.cmd` on `tests/symidx/b3.c`, not new cases in
  `csym.cmd`.
- **Completion item kinds** are named `CK_*` in features.c (the old
  `CIK_FUNCTION`/`CIK_CONSTANT` clashed with `CIdxKind`); one table
  (`C_KINDS`) gives the completion kind, symbol kind and token type per
  `CIdxKind`.
- **A file that includes itself** (gcc.dg/range-test-1.c, found by
  symcov): a block of the second inclusion has the same file index as the
  function body around the `#include` but lies after it, so clamping it
  to its parent left it outside (`--verify-symbols`: "scope N is not
  inside its parent"). A block that does not overlap its nearest kept
  ancestor is now dropped like one in another file (its names take the
  ancestor); `tests/symidx/selfinc.c`.
- **System functions** answer signature help from their header text
  (checked by hand on glibc's `strlen` and `printf`, not a golden: the
  text depends on the host's headers).

**Measurements** (`--verify-symbols` totals, old binary against new):

| Unit | Before | After |
|---|---|---|
| src/main.c | 3,453 decls, 352 KB | 4,008 decls, 461 KB (1,733 scopes) |
| src/c/cexpr.c | 5,330 decls, 704 KB | 6,297 decls, 918 KB |
| zstd.c (amalgamation) | 12,717 decls, 1.83 MB | 14,207 decls, 2.52 MB (27,411 scopes; 15 to 54 files) |
| 4 MiB of `int fN;` members | 29.97 MB | 32.6 MB |
| 4 MiB of `int vN;` declarations | 23.3 MB | 31.2 MB |

The decl counts grow by the eager system declarations (3.4) and the
nameless decls; the file count by headers that now hold a kept scope.
Both dense files end slightly over the ~30 MB budget, as section 7
expected for the second; accepted. Putting `scope` and `parent` in one
field would save about 1.3 MB on the first; not done, and the premise
was wrong: an enumerator of a block-scope enum needs both (`visible`
reads its scope). Exact merging needs a side table for those; parked as
not worth about 4 bytes per decl. Index build time: zstd.c `--verify-symbols` best of 7,
two rounds, 0.161/0.149 s before and 0.161/0.134 s after: no measurable
change (machine shared, about 56% CPU load; Defender real-time
protection off).

Size: 1,056 lines added and 161 removed in src (csymidx.c +480/-61,
features.c +446/-45, main.c +55/-21, server.c +33/-29, csymidx.h +32/-4,
check.c +4, lsp.h +4, crename.c +2/-1), against the ~550 estimated in
section 8: documentSymbol, the signature parameter scan and the index
verification of scopes took about twice their estimates.

