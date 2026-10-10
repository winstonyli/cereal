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

## 12. Carry-over design

Status: designed 2026-10-09 for the parked item of section 9 ("carrying
the previous index across edits"). Step 1 of 12.11 (the index side) is
implemented (Round 198, results in 12.13); steps 2 to 5 are not. When all
of it is implemented it
replaces the "No carry-over" bullet of section 6 and both the carrying
and the `workspace/semanticTokens/refresh` items of section 9; those
bullets are left as they are until then. Line numbers are those of HEAD
2c7e406.

### 12.1 Problem, and what the old rejection missed

After an edit the new macro snapshot is installed without a C index;
the index arrives with that snapshot's check (0.05 s for main.c, 0.34 s
for zstd.c, 1.7 s for a dense 4 MiB table; LSP.md). Meanwhile every
request with the `cidx` column (server.c:1030) runs the D1 wait: up to
1.5 s, then the macros alone. Two costs follow:
- **Flicker and gaps.** Hover, definition, completion and highlight say
  nothing about C names after a timeout; semantic tokens lose every C
  token and are not asked for again (section 6).
- **A blocked protocol loop.** Requests run synchronously on the one
  protocol thread (`lsp_main` calls `handle_request`; there are no
  handler threads). While a request waits, the next `didChange` is not
  even read, so the check it would cancel runs on. Fast typing in a large
  unit queues keystrokes behind token and highlight requests.

Section 6 rejected carry-over because it "would retain a second snapshot
per unit" and the newest index is good on broken code. The second point
stands, but the gap is latency, not broken code. The first does not
hold for this design: only the C index is kept, not the snapshot. The
mapping is computed at install, when the builder already holds both
snapshots.

### 12.2 Decisions

- **K1. A carried index is an ordinary CIndex in the new text's
  coordinates.** At install, the builder makes a copy of the previous
  index's per-text arrays (events, CSR, scopes, files), with offsets
  shifted past the edit and the events that overlap the edit dropped.
  It shares the decls and strings with the original. Every query in
  csymidx.c and every feature in features.c then works unchanged, with
  no special cases per request. Rejected: a view that maps positions at
  query time. It would leave the shared index's `stale` flags wrong
  for the new text (`cindex_validate` writes them into the index). It
  would need a map at about 12 call sites in features.c and a filter
  inside `cindex_visible` (a damaged inner declaration would otherwise
  shadow a valid outer one). It would also answer nothing anywhere in
  the edited span, including completion at the cursor.
- **K2. The edit is the difference between the two snapshots' texts,
  not a log of `didChange` ranges.** For each file of the index: the
  common prefix and suffix of its text in the old and new snapshot
  (12.3). That covers every way a text changes: range and whole-text
  `didChange` (`tests/lsp/carry` sends whole texts and still gets the
  minimal span), `didClose` back to the disk, a header edited on disk. It
  needs no bookkeeping shared between threads, and chains compose by
  construction, since each carry is relative to the snapshot before.
  Cost: several separate edits between two installs (multi-cursor
  edits, a formatter) merge into one damaged span. The same edit drives
  the diagnostics carry-over (12.8), replacing `first_changed_line`.
- **K3. Lifetime by reference count, freshness by serial.** `CIndex`
  gets an atomic `refs` (and `cindex_free` drops one), a `serial` unique
  per index, and `core`, the original that owns decls and strings (a
  carried index holds one reference on it). A snapshot's `cidx` is no
  longer set once: carried at install, replaced by its own at publish.
  So a request takes a reference under `S.m` and drops it when it
  answers.
- **K4. A wait policy per request, replacing the `cidx` boolean
  (12.6).** A request waits for the newest edit's *snapshot*, which it
  needs so that positions match. It waits for the *check* only where the
  carried index cannot answer: when there is nothing to carry, at a
  position inside the damage (hover and the go-to requests), and where
  only a fresh or complete answer will do (references, rename).
- **K5. The client is told about stale answers where LSP has a way to
  say so.** `workspace/semanticTokens/refresh` after a check publishes
  if a token answer since the last refresh lacked the fresh index (only
  for clients with `refreshSupport`; the others keep D1 for tokens).
  Completion from a carried index is `isIncomplete: true`. Hover adds a
  line when the entity's own declaration was edited (12.7).

### 12.3 The text edit

```c
typedef struct CIdxEdit {   /* old [pre, old_end) became new [pre, new_end) */
    uint32_t pre, old_end, new_end;
    bool same;              /* identical texts */
} CIdxEdit;
void cindex_text_edit(const char *a, size_t na, const char *b, size_t nb,
                      CIdxEdit *out);
```
- **Prefix and suffix.** `pre` is the common prefix length. The common
  suffix length is capped so that `pre + suffix <= min(na, nb)`. This
  is one `memcmp`-like pass from each end; for a unit under the 4 MiB
  check limit, about a millisecond.
- **Widening to whole identifiers.** If an identifier crosses a span
  boundary in either text, the span grows to contain all of it in both
  texts: the run of `cindex_ident_char` bytes ending at `pre` moves
  `pre` back when the byte after the boundary is an identifier byte in
  the old or new text, and the same holds forward at the suffix. So
  `foo` changed to `foobar`, `x` typed before `foo` (`xfoo`), and an
  insertion inside `foo` all damage `foo`. Typing `;` after `x` does not
  damage `x`. Events are identifier spellings (argument, body,
  invocation name), so after widening no event straddles a boundary.
- **The map** `f` of an old position `x` (a point between bytes):
  `x <= pre` gives `x`; `x >= old_end` gives `x - old_end + new_end`;
  inside the span it gives `pre`. `f` is monotone. An event `[off,
  off+len)` is **kept** if `off + len <= pre` or `off >= old_end`, else
  **dropped**. The **damage** of the file is `[pre, new_end]` in the new
  text, inclusive at both ends, so the cursor at either edge of the typed
  text counts as inside.

### 12.4 Carrying an index (csymidx.c)

`CIndex *cindex_carry(CIndex *from, SrcMgr *old, SrcMgr *new)`. `from`
is the old snapshot's index, own or itself carried, whose offsets are in
the text of `old`.
- **Files.** Each `from` file is looked up by path in both source
  managers (`cindex_srcfile`) and its edit computed. If `from` already
  marks it stale, or `new` lacks it, it stays or becomes **stale** (its
  events are kept but never answered, as today). Otherwise it is
  current: `size` and `hash` describe the new text, and the damage is
  the hull of the new edit's damage and the previous damage mapped
  through `f` (one span per file; it only grows along a chain of
  cancelled checks). `CIdxFile` gains `edited`, `dmg_begin`, `dmg_end`.
- **Events** in order: kept ones get `f(off)`, dropped ones are left
  out. Order is preserved because `f` is monotone. The CSR (`by_decl`,
  `by_decl_start`) is rebuilt by the code of `csx_finish`
  (csymidx.c:1131-1143), moved into a function both use. Decl indices
  do not change, so `decls` is shared.
- **Scopes** keep their indices, so `CIdxDecl.scope` and `parent` stay
  valid: `begin = f(begin)` and `end = f(end)`, with no re-sort. A
  monotone map keeps the (begin ascending, end descending) order weakly
  and keeps scopes nested or disjoint (a scope may become empty), so the
  correctness argument of `cindex_scope_at` (3.2) still holds. A
  function body that contains the edit grows over it. A scope whose
  `}` was inside the span ends at `pre`: an approximation until the
  check, used only by completion's scope test and documentSymbol
  ranges.
- **Identity.** When no file changed, `cindex_carry` returns `from` with
  one more reference and no copy.
- **Shared and copied.** Copied: files, events (12 B), `by_decl` (4 B
  per kept event), `by_decl_start` (4 B per decl), scopes (16 B).
  Shared: decls (24 B) and the string pool (names, paths, hover texts).
  A decl whose events were all dropped stays without events. The queries
  already skip such decls: `cindex_visible` needs a DECL/DEF event, and
  documentSymbol walks events. `cindex_verify` therefore skips its
  "every decl has a declaration event" rule for a carried index.
- **Damage query.** `bool cindex_damaged(const CIndex *, int fi, uint32_t
  off)`. **Touched query.** `bool cindex_touched(const CIndex *, uint32_t
  d)` is true when a carried index has fewer DECL/DEF events for `d` than
  its core (a scan of the decl's two CSR lists): the declared name
  itself was edited. It misses edits next to an intact name: `int x`
  changed to `long x` damages only `int`/`long`. features.c therefore
  also counts a DECL/DEF event that lies on a line the damage touches
  (12.7).

### 12.5 Server: when to carry, publish, fail (server.c)

- **Carry at install.** The builder is the only writer of `u->snap` and
  of every `Snapshot.cidx`, so it may read `old = u->snap` and
  `old->cidx` before taking `S.m`. After `build_unit`, it runs
  `check_eligible` (moved before the lock; it touches only the
  unpublished snapshot). If the new snapshot will be checked and
  `old->cidx` exists, it runs `cindex_carry(old->cidx, &old->tu.sm,
  &snap->tu.sm)`. Then, under the lock, at install (server.c:732-743),
  it sets `snap->cidx` to the carried index and `snap->cidx_carried`. If
  the snapshot is not installed (`want <= u->built`), the carried index
  is dropped with it. No carry for a snapshot that will not be checked
  (over the limit, a header alone): no fresh index would replace it.
- **Chains.** If the check of S1 is cancelled by edit 2, S1 keeps its
  carried index with `CHECK_DONE` (server.c:790 marks a cancelled check
  done), and S2 carries from S1's carried index. The damage accumulates,
  and the core stays the last index a check produced.
- **Publish** (server.c:777-789): under the lock, `prev = checking->cidx;
  checking->cidx = c->cidx; checking->cidx_carried = false`. After
  unlocking, `cindex_free(prev)`. A request that still holds `prev`
  keeps it alive through its reference.
- **Failed check, not superseded** (`fatal()`, the `else if` at
  server.c:783): the carried index is dropped too, like the carried
  diagnostics. The snapshot then answers with macros only, as today.
  Otherwise stale entities would stay on screen indefinitely.
- **Rename** stays on fresh indexes only. `r.c_fresh` (server.c:1103)
  also requires `!cidx_carried`. That is already implied, since a
  carried index is never on a snapshot whose check ended uncancelled,
  but it is stated here so that it does not depend on that.

### 12.6 Request policy and waits (server.c)

The `cidx` boolean of `REQS` becomes a policy:

| Policy | Requests | Waits (D1 deadline, 1.5 s) for | Answers from |
|---|---|---|---|
| `CP_NONE` | foldingRange, call hierarchy, `cereal/expandMacro` | nothing | macros |
| `CP_ANY` | completion, signatureHelp, documentHighlight, documentSymbol; the three semantic token requests when the client has `refreshSupport` | the newest edit's snapshot (`gen == want`); then the check too, but only if that snapshot has no index at all (first build, or nothing to carry) | own or carried index |
| `CP_POS` | hover, definition, declaration, typeDefinition | as `CP_ANY`; then the check too if the request's position is in the carried index's damage | own or carried |
| `CP_WAIT` | references, prepareRename, rename; semantic tokens without `refreshSupport` | the check, as D1 today | own; after a timeout carried (references and tokens); rename refuses ("retry") |

- **Why these.** Completion, signature help and highlight run while
  typing, with the cursor in the damage, and blocking them stalls the
  loop. They answer well from a carried index: completion's scope test
  works in the damage (12.4), and signature help falls back to the
  callee's name through `cindex_visible` (5.3 step 3). Hover and the
  go-to requests are deliberate, so a short wait beats a null for a name
  just typed. References and rename are lists the user acts on, so
  they must be complete. documentSymbol is `CP_ANY` because LSP has no
  way to refresh it. The outline then lacks declarations typed in the
  damage until the client asks again (open question 2).
- **The position test** (`CP_POS`), under `S.m`: the request's offset
  in the snapshot's file (`index_find_file`, `req_loc`) and
  `cindex_damaged`. If it is damaged, wait on `S.done` with the same
  deadline, then take `u->snap` again.
- **References after a timeout** answer from the carried index instead
  of the macros alone: positions are right, though uses typed in the
  damage are missing. Behaviour change; open question 1.
- **Lifetime.** `r.cidx = cindex_ref(snap->cidx)` under `S.m`; released
  after the response (`Req.cidx` loses `const`). `Req` gains `c_carried`.

### 12.7 What the client sees (features.c)

- **Semantic tokens.** `TokCache.cidx` (a bool, features.c:692) becomes
  the index's `serial` (0: none). A whole-file result made from a
  carried index is then not reused after the own index publishes. This
  also settles the reused-pointer worry of 5.4. When a token request is
  answered without the fresh index (carried, or none after a timeout),
  the server sets `Unit.tok_stale` under `S.m` while setting up the
  request. At the next publish of a fresh index for that unit, the
  builder clears it and, if the client declared
  `workspace.semanticTokens.refreshSupport`, sends
  `{"jsonrpc":"2.0","id":"cereal-refresh-N","method":"workspace/semanticTokens/refresh"}`.
  It sends under `S.m`, as it already does for `publishDiagnostics`;
  `rpc_write` has its own lock. The client's response has no `method`
  and is already skipped (server.c:1311). This settles the section 9
  item "refresh after a check that ended past the D1 wait". A client
  without `refreshSupport` keeps D1 for tokens (`CP_WAIT`). Cost per
  refresh: the client asks again for each visible document of the
  server. The delta fast path (features.c:735-746) answers unchanged
  documents with empty edits, so the 35 MB macro files (never checked,
  so never a cause of a refresh) cost only that.
- **Overlap of a macro token and a carried C token.** Fresh, they never
  start at one place (5.4). A carried index can break that: `int foo;`
  stays a C DEF event while a new `#define foo` above makes it a macro
  use in the fresh macro index. `STok` gains its source, and
  `stok_cmp` puts macro-index tokens first at a tie. Fresh output is
  unchanged.
- **Completion** from a carried index sets `isIncomplete: true`, so the
  client asks again on the next keystroke and gets names declared in
  the damage once the check has published.
- **Hover** from a carried index adds "(rechecking: its declaration was
  edited)" when the entity's declaration was edited. That means
  `cindex_touched` is true, or one of its DECL/DEF events is on a line
  the damage touches (lines from the snapshot's line table). So `int x`
  changed to `long x` is marked wherever `x` is hovered; a type written
  on the line above the name is not. The hover texts are the old
  check's.
- No other marker: documentSymbol, highlight and signature help have no
  field for staleness. `CEREAL_LSP_STATS` logs `carried` per request
  and `carry %.3fs, kept N of M events` per install.

### 12.8 Diagnostics through the same edit

`carry_cdiags` (server.c:367) keeps the previous compiler diagnostics
that lie wholly before the first edited line (`first_changed_line`,
server.c:338), and keeps one with a note in another file only if no
file changed. With 12.3 the same edit can carry the ones after the edit
too:
- `CDiag` stores file offsets instead of the rendered JSON: range, notes
  (path and offset), severity, code, message and note texts. Offsets are
  kept only for files whose checked text equals the snapshot's (the
  case `cindex_validate` checks); others are not carried.
- `carry_cdiags` maps every offset of a diagnostic (range, notes in any
  file) through that file's edit. It drops the diagnostic if any offset
  is damaged or its file is gone. `lsp_publish_diagnostics` renders
  carried ones against the new snapshot's line table and computes their
  keys there. `first_changed_line` and the "no file changed" rule go:
  an unchanged file has the identity edit.
- **Effect:** errors below the edited line stay in place, moved, while
  typing, instead of vanishing until the check ends. `tests/lsp/carry`
  changes: the diagnostic below the edit is now kept and moved.

### 12.9 Memory

Index memory per unit (measured totals from section 11; the split
between shared and copied parts is estimated from field sizes and is to
be measured in step 1 of 12.11):

| Unit | Index | Copied part | Shared part |
|---|---|---|---|
| zstd.c | 2.52 MB | ~1.5 MB (about 66k events, 27.4k scopes) | ~1.0 MB |
| 4 MiB of `int vN;` | 31.2 MB | ~12 MB (330k events and roots) | ~19 MB |
| 4 MiB of `int fN;` members | 32.6 MB | ~7 MB (one root) | ~26 MB |

- **Steady state:** unchanged, one index per unit (the ~30 MB budget of
  B2, already slightly exceeded by the dense files, section 11).
- **During the check:** today the old index is freed when the old
  snapshot goes. Now one index's worth stays (core plus copy). The worst
  case at the 4 MiB limit is +31 MB. The only check-phase peak on record
  is +194 MB over the macro phase, for a dense 4 MB single-initializer
  table (LSP.md, "Size limit"), a different file from the index's worst
  ones. Against that, +31 MB is about +16%. Step 1 of 12.11 measures
  peak RSS with and without carry on the two dense index files. No new
  limit is expected; if that measurement says otherwise, a cap on the
  carried index's size is the fallback.
- **Transients:** at install, the old own index plus the new copy until
  the old snapshot is released (+12 MB worst). At publish, the new own
  index plus the carried one until the unlock and the end of any request
  holding it (+31 MB worst, for milliseconds).
- **Time:** the copy plus diff plus hash at install is O(events +
  scopes + text): about 1 ms for zstd.c and ~10 ms for the dense 4 MiB
  files, against checks of 0.34 s and 1.7 s. It runs outside `S.m`.

### 12.10 What it cannot cover

- **Effects at a distance until the check publishes:** an edited
  typedef, declaration type, `#define` used by C code, `#include` or
  `#if` changes the meaning of unchanged text elsewhere. Those places
  answer with the old meaning, correctly placed (hover marks only the
  edited declaration itself). A new shadowing declaration is in the
  damage, so it is not seen.
- **The damage itself:** names written there have no hover, definition
  or highlight (hover and definition wait), no C tokens, and are not
  offered by completion until the check.
- **Separate edits merge** into one span (multi-cursor, formatters): a
  whole-file reformat damages nearly everything, which is today's
  behaviour.
- **An expansion with edited arguments** keeps its at-expansion events
  when the invocation name lies outside the damage.
- **Files newly included** have no events until the check.
- **Units that are not checked** (over 4 MiB, a header alone) carry
  nothing, as before.
- **documentSymbol** cannot be refreshed by the server.

### 12.11 Tests, order and estimate

1. **Index (done, Round 198; see 12.13).** `refs`, `serial`, `core`; the CSR moved out of
   `csx_finish`; `cindex_text_edit`, `cindex_carry`, `cindex_damaged`,
   `cindex_touched`; the verify exception. The runnable check is
   `cereal check --verify-carry=OLD FILE`. It indexes FILE with OLD's
   text as an overlay at FILE's path, then FILE itself, carries the
   first to the second, and runs `cindex_verify` on the carried index:
   sorted events, CSR, scopes sorted and nested, hashes against FILE.
   Outside the damage, every carried event must match a fresh event at
   the same offset with the same role, name and kind, and the reverse.
   It prints `carry: kept K of N events, damage L:C-L:C, D differences`.
   Goldens go in `tests/symidx`: a comment line inserted at the top (no
   differences), an identifier extended (`foo` to `foobar`: damage
   widened), a statement typed in a body, a `}` deleted (scopes clamped;
   differences reported, not an error), a typedef changed to a variable
   (differences at a distance, reported), and an edit in a header. The
   cells' differential fuzzer (PARALLEL.md) could run it on random edits
   (parked).
   Measure the copied and shared parts and the time on zstd.c and the
   two dense files (12.9).
2. **Server and features.** Carry at install, publish swap, failure
   drop, references in requests, the policy column, `TokCache.serial`,
   the tie rule, `isIncomplete`, the hover line. A test extension,
   `cereal/holdChecks {"hold": bool}`, makes a check wait at its start
   (on `S.work`) while held; an edit's cancel, a release or shutdown
   ends the wait. A request sent right after a `didChange` waits for the
   new snapshot (`CP_ANY`), so with checks held it answers from the
   carried index deterministically, with no new barrier.
   `cereal/waitIdle` must not be used while held: it would wait for the
   check. New session `tests/lsp/carry_cidx`: hold, then an edit that
   inserts a line above and types a declaration and a call in a body.
   Then hover and definition below the edit (moved), completion in the
   body (`isIncomplete` true, without the typed local), whole-file
   semantic tokens (none in the damage, the rest moved), documentSymbol
   (ranges moved), and signature help on the typed call (by name). Then
   hover on the typed name (`CP_POS`: waits out the 1.5 s while held,
   then nothing) and rename ("retry" after the wait). Then a second edit
   while held (a carry from a carried index; damage hull). Then release,
   idle, the `workspace/semanticTokens/refresh` request recorded (the
   session declares `refreshSupport`), a delta with the damage colored,
   and completion `isIncomplete` false with the local. `fault_check`
   gains a carried index dropped on a failed check.
   `tests/lsp_session.py` must accept a server request without `params`
   in its `wait` step (`dict(m.get("params") or {})`). In csym,
   csym_hover and csym_refs, the D1 notes change wording: hover,
   definition and highlight no longer wait for the check. Their answers
   do not change, since a line inserted at the top moves every event
   alike. The B2 claim "csym_hover fails without the wait" then no
   longer applies; the wait is covered by `CP_POS` in `carry_cidx`.
3. **Refresh.** The capability in `initialize`, `Unit.tok_stale`, the
   send at publish, the token policy chosen by the capability. Tests:
   `carry_cidx` (with the capability: the refresh is recorded) and
   `csym_b3` (without it: no refresh is sent). `csym_b3` also gains a
   token request right after an edit: it waits for the check and has
   the new names, which makes the `CP_WAIT` choice for tokens visible.
   The capability is per session, so it cannot go in `carry_cidx`.
4. **Diagnostics through the edit (12.8).** `CDiag` by offsets,
   `carry_cdiags` rewritten, `first_changed_line` deleted;
   `tests/lsp/carry` updated.
5. **Docs.** LSP.md ("Snapshots", carried diagnostics, D1, Tests),
   sections 6 and 9 here, HISTORY.
- **TSan and ASan.** Every session runs under ThreadSanitizer and
  AddressSanitizer/UBSan as today. `carry_cidx` exercises the release of
  a carried index by both threads. A non-golden stress run (200 edits
  each followed at once by tokens, completion and hover on zstd.c,
  checks not held) must leave the sanitizer logs empty. The interleaving
  of a request holding the carried index while the builder publishes is
  timing-dependent; this run is the check for it.
- **Estimate:** about 400 lines for steps 1-3. csymidx.c/h ~170
  (refcount and core ~25, text edit with widening ~35, carry ~75, damage
  and touched ~20, verify ~5, CSR move ~10). main.c `--verify-carry`
  ~50. server.c ~140 (carry, publish and failure ~45, policy and waits
  ~40, references ~8, holdChecks ~25, refresh ~25). features.c ~35
  (serial, tie rule, `isIncomplete`, the hover line with its line test).
  lsp.h ~10. Step 4 adds ~80 lines and removes ~60 (server.c and
  features.c). B3's own parts took up to twice their estimates (section
  11), so allow up to ~800 in all.

### 12.12 Review rounds

Seven rounds were run on this section against the code at 2c7e406.

1. **Claims against the code.** Found: the brief's "handler threads"
   do not exist (one protocol thread answers synchronously, and the D1
   wait blocks it, which 12.1 now states). `cindex_validate` writes
   `stale` into the index itself, so a shared index cannot be valid for
   two texts (this decided K1 against a view). `TokCache` keyed by a
   bool would reuse a carried result after publish (keyed by serial).
   `CHECK_DONE` is also set for a cancelled check, so a carried index
   can sit on a "done" snapshot (`c_fresh` stated to exclude carried).
   The `int vN;` copy estimate first left out `by_decl_start`.
2. **Races and lifetime.** Found: `snap->cidx` changing after
   publication needs references held by requests (K3). Before, it was
   set once and the snapshot reference sufficed. A carry computed
   before the lock may belong to a snapshot that is then not installed
   (dropped with it). `waitIdle` deadlocks against held checks
   (documented in the test plan). A failed check would leave the carried
   index for good (now dropped, like the carried diagnostics).
   Confirmed: the builder is the only writer of `u->snap` and `cidx`, so
   it reads them without the lock. The refresh is sent under `S.m` as
   `publishDiagnostics` is. Client responses are already ignored.
3. **Mapping edge cases.** Found: an identifier extended at the edit
   (`foo` to `foobar`), or a character typed before it, left an event on
   a changed name (widening, 12.3). A cursor exactly at a span edge was
   neither inside nor outside (damage made inclusive). Mapping scope
   begins and ends to different points of the span can break nesting
   (one monotone `f`; nesting argued). A macro token and a carried C
   token can now start at one place (tie rule). A whole-text
   `didChange` would defeat an edit log (one of the reasons for K2).
   Walked: chains (each carry relative to the snapshot before), stale
   source files, files missing from the new snapshot, identity carries,
   decls left with no events, prototype and body scopes around the
   edit.
4. **Memory.** Found: the first draft counted only steady state. The
   during-check and transient figures (12.9) were added, and set
   against the check's own peak, which sets the 4 MiB limit.
5. **Consistency.** Found: the estimate's parts summed to about 400,
   not the 450 stated. The during-check memory set the index's worst
   files against the check peak of a different file (now said, with a
   measurement added to step 1). Checked: the policy table against the
   D1 tests whose wording changes, and 12.10 against 12.6 (hover waits
   in the damage, highlight does not).
6. **Staleness markers and tests.** Found: `cindex_touched` alone misses
   the most common stale hover (`int x` changed to `long x` keeps `x`'s
   event), so a line test was added. A token test planned for
   `carry_cidx` needed the client without `refreshSupport`, but the
   capability is per session, so it moved to `csym_b3`.
7. **Final pass** over the whole section: nothing material. Sections 6
   and 9 are named as superseded but not edited.

### 12.13 Step 1 results (Round 198)

Implemented as designed (csymidx.c/h, main.c, `tests/symidx/carry`);
`refs` is a `uint32_t` updated with the `thread.h` atomics. Differences
from the text above:
- `cindex_verify_carry(ix1, c, fresh, sm2, out)` lives in csymidx.c (it
  needs the file-local helpers) and takes the carried index, which
  main.c makes and times; `--verify-carry=OLD[@PATH]` lets OLD stand in
  for PATH instead of the input (the header golden). Its output is the
  `carry:` line, up to 20 `diff:` lines (events, then scopes outside the
  damage; a scope is outside when neither end is in it) and any
  `verify:` problem; only structural problems set the exit status. It
  also checks `cindex_damaged` at and next to both damage ends and
  `cindex_touched` against the event counts.
  `CEREAL_CARRY_STATS=1` prints sizes and the carry time on stderr.
- `cindex_bytes` of a carried index counts only what it owns.
- Goldens: `tests/symidx/carry/NAME.{old,c,expected}` (`NAME.at`: the
  `@PATH`), run by a loop of its own in tests/run.sh: `top_comment`,
  `ident_extend`, `stmt_typed`, `brace_deleted` (3 scope differences),
  `typedef_var` (16 differences at a distance) and `hdr` (an edit in a
  header). The tool does detect a bug: shifting kept offsets by one
  byte fails `top_comment` at once.

**Measurements** (`cereal check --verify-carry`, one inserted line in the
middle; the dense files regenerated as in section 11; machine shared;
bytes):

| Unit | Index | Copied | Shared | Index + copy | Copy time |
|---|---|---|---|---|---|
| src/main.c | 476,413 | 187,480 | 288,933 | 0.66 MB | 0.6 ms |
| zstd.c | 2,523,151 | 1,572,920 | 950,231 | 4.10 MB | 1.1 ms |
| 4 MiB `int vN;` | 31,209,205 | 11,910,944 | 19,298,261 | 43.1 MB | 4.4 ms |
| 4 MiB `int fN;` | 29,608,277 | 6,144,640 | 23,463,641 | 35.8 MB | 2.6 ms |

The copied and shared parts agree with the estimates of 12.9 (1.5, 12 and
7 MB copied). The time includes the text diff and the hash. RSS growth
over the carry (+5.4 MB for `vN`, +1 MB for `fN` and less elsewhere) says
little, since malloc reuses freed pages.

**The memory claim is too low.** 12.9 says one extra index alive during
the check costs +31 MB (`vN`, +16% of the 194 MB check peak on record).
As built (K3), the copy holds a reference on the whole original, so its
events, CSR and scopes live on with the decls and strings: the extra is
index plus copy, 43.1 MB for `vN` and 35.8 MB for `fN`. Against peak RSS
of `cereal check --verify-symbols` on the same file (154 MB and 230 MB)
that is +28% and +16%; for zstd.c, +4.1 MB on 17 MB. The server's own
check peak (macro phase included) was not measured, since the server
side is step 2. To get back to +31 MB the carried index would keep what
`cindex_touched` needs (a DECL/DEF count per decl, 4 bytes) and the core
would shrink to decls and strings, a separate refcounted object. Not
done; the choice (that, or accept +43 MB, or a size cap) is open.

**A limit found.** An inserted line that begins like the next
declaration (`int inserted_decl;` before `int v165428;`) makes the text
diff start inside that declaration, so its root scope grows over the
inserted line and `--verify-carry` reports one scope difference. It is
the same approximation as an edit inside a declaration, but it fires for
line insertions that share a prefix with the following line; completion's
scope test is the only user. `static int inserted_decl;` has none.

