---
status: done
---

# B4 design: C call hierarchy, parameter-name inlay hints, workspace/symbol over open units

Status: designed 2026-10-10 (ROADMAP Track B, B4). **B4a (C call hierarchy)
is implemented** (Round 218; section 17 lists what differs from this text),
and so is **B4b (parameter-name inlay hints)**, with `bench/tools/clangd_diff.py`
(Round 219; section 18), and **B4c (workspace/symbol over open units)**
(Round 220; section 19). **B4 is complete**; the parked items are in 15.
It builds on the C symbol index of B2 and B3 (B2_DESIGN.md, B3_DESIGN.md;
LSP.md, "C symbol index"). Line numbers refer to the Windows checkout at
809a6c8 (drafted at 82c44b2; every cited line was rechecked after
809a6c8 changed server.c). Behaviour claims about clangd come from probes of
`/usr/bin/clangd` in WSL, which is **clangd 20.1.8** (`clangd-20`), not
18 as the task brief assumed (section 10.1).

## 1. Goals

| Request | Today | After B4 |
|---|---|---|
| `textDocument/prepareCallHierarchy` | macros only | a macro first, else the C function or file-level variable at the cursor |
| `callHierarchy/incomingCalls` | macro graph edges | plus, for a C item, the calls of it in this unit, grouped by the calling function (or the variable whose initializer calls) |
| `callHierarchy/outgoingCalls` | macro graph edges | plus, for a C item, the functions called in its definition (or a variable's initializer), grouped by callee |
| `textDocument/inlayHint` | not advertised | parameter-name hints (`count:`) before call arguments |
| `workspace/symbol` | not advertised | the C declarations and macros of the units that have an open document, fuzzy-matched; partial by design until B5 |
| `cereal query callers\|callees` | macros only | plus C names |

Macros keep precedence everywhere, as in B2 and B3: a name the macro index
answers for strongly is a macro.

## 2. Decisions

D1 to D6 are the user's and are not reopened here.

- **D1.** B4 is the C call hierarchy, parameter-name inlay hints and
  workspace/symbol over open units only. Type and chain hints and the type
  hierarchy are dropped.
- **D2.** Call data is a `CIX_CALL` flag on events of the existing CIndex
  (`src/c/csymidx.[ch]`), set at the call hook in `cexpr.c`: the direct
  callee identifier of an `N_CALL`. Indirect calls are not edges. Outgoing
  calls are the flagged events inside the function's body. Incoming calls
  are the flagged events of the decl, grouped by the enclosing function
  through the scope tree.
- **D3.** B4 comes before the B5 design.
- **D4.** workspace/symbol covers the open units now and is documented as
  partial.
- **D5, D6.** B5 (a project index of on-disk per-TU summaries keyed by
  file hashes, with cross-TU rename last) is out of scope. Two parts are
  in: a short forward-compatibility section (section 11) on what B4 must
  not preclude, and a 1-hour spike on the checker's static state before
  the B5 design.

Decisions this design makes, each argued in its section:

- **E1.** `CIX_CALL = 64`. It goes on the REF event of a name that is the
  syntactic callee of an `N_CALL` after parentheses are stripped, whatever
  the entity's kind (section 4.1; field callees pending Q7). Call
  hierarchy reads only `CIK_FUNC` callees. Inlay hints read every kind
  whose parameter list resolves.
- **E2.** Calls whose callee name is spelled in a `#define` body are also
  recorded at the invocation in the source (the presentation point that
  pasted names already use), in a side array `xcalls` (section 4.4). This
  is an extension of D2 and needs the user's go-ahead (Q1).
- **E3.** The calling entity of a site is decided by its root scope (its
  external declaration): the function the root defines, else the
  file-level declarator before the site (section 5.1).
- **E4.** Follow-up requests find their unit through `item.data.doc`, the
  document that answered prepare. This fixes the follow-ups for items in
  unopened headers, which already fail for macro items today (section 5.5).
- **E5.** Inlay-hint parameter names come from the index's parameter
  events inside each parameter's text span. A text rule is used only for
  callees declared in system headers, which have no parameter events
  (section 6.3, Q3).
- **E6.** workspace/symbol has its own handler. It reads whatever index
  each open unit has right now and never waits or starts a check (section
  7). This deviates from the brief's "policy CP_ANY", because CP_ANY can
  wait up to 1.5 s per unit.

## 3. What exists (verified in the code)

**Index**

- **Events.** `CIdxEvent {off, decl, file, len, flags}` is sorted by
  (file, off) (csymidx.h:58). The flags are the role in the low 2 bits
  (`CIX_DECL` 0, `CIX_DEF` 1, `CIX_REF` 2), then `CIX_MACRO_BODY` 4,
  `CIX_AT_EXPANSION` 8, `CIX_ARG` 16 and `CIX_SYSTEM` 32. Bits 64 and 128
  are free.
  - Build-only aliases: `ROLE_TENT` = 3, and `FROM_MACRO` = `CIX_ARG`
    until finish classifies the event.
  - `push_ev` ORs the role argument into the flags (csymidx.c:336), so a
    caller can pass `CIX_REF | CIX_CALL` unchanged.
- **Placement** (`where`, csymidx.c:311; B2_DESIGN.md 5.3):
  - a token not from a macro is placed at its spelling;
  - a pasted or synthesized token is placed at its presentation point
    `p->exp` with `CIX_AT_EXPANSION` and len 0, measured at finish;
  - any other macro token is placed at its spelling as `FROM_MACRO`. At
    finish it becomes `CIX_MACRO_BODY` if it lies in a `#define` range
    (csymidx.c:1040), else it stays `CIX_ARG`.
  - Events presented in a system header are dropped. A system decl used
    by user code gets one lazy event at its declaration
    (`AT_EXPANSION`, plus `CIX_SYSTEM` at freeze).
- **Placement probe** (`cereal check --dump-symbols` on a scratch file).
  A call spelled in a macro body, `#define CALL_LEAF(v) leaf(v, 0)`, has
  only `4:22 REF func leaf body`: nothing at the invocation.
  - So the brief's premise that calls through macros land at the
    invocation with `CIX_AT_EXPANSION` holds only for names formed by
    `##`.
  - LSP.md's "else at the invocation (`expansion`)" (lines 186-188) is imprecise
    for the same reason. Corrected in LSP.md with B4a.
- **Dedupe.** `csx_finish` sorts the events with `ev_cmp` (loc, decl,
  role; csymidx.c:717). `qsort` is unstable. It then drops an event equal
  to its predecessor (csymidx.c:1093-1094). Flags other than the role do not
  take part.
- **Scopes** (csymidx.c:576-611, 908-980). Every external declaration gets
  a root scope `[first token, last token)` at presentation points
  (`csx_unit_begin`). Blocks, prototypes and function bodies are its
  children (`csx_scope`).
  - `freeze_scopes` drops a root that is not inside one user file, and
    drops the children of a dropped root.
  - Queries: `cindex_scope_at`, `cindex_scope_root`, `cindex_lower`,
    `cindex_at`, `cindex_decls_at`, `cindex_select`, `cindex_visible`.
  - Nothing marks which child is the function body.
  - `CIdxDecl.scope` is 0 for file-level names.
- **Carry and the verifiers.**
  - `cindex_carry` (csymidx.c:1773) copies each event whole, shifting the
    offset and dropping the events that overlap the edit, so new flag
    bits survive it.
  - `--verify-carry` compares (off, role, kind, name) per event
    (`VEv`, csymidx.c:1890). It does not compare flags.
  - `--verify-symbols` (`csx_unit_end`, csymidx.c:671) reports a naming
    node whose token got no event, unless `excused`. `__builtin_*` names
    are excused. Tokens with an event are marked in `b->seen`.
  - `cindex_print_flags` (csymidx.c:1581) is shared by `--dump-symbols`
    and `cereal query` C output (main.c:1245).
  - crename.c:281 compares whole `flags` of the events before and after a
    rename check, so a rename that would change call-ness is refused. That
    is correct; it cannot happen without a macro changing.

**Checker hooks**

- `e_ident` (cexpr.c:1989; the call at 2060) calls `csx_sym(c, ref, tok, CIX_REF)`.
- `is_callee` (cexpr.c:1689) is true when the parent is `N_CALL` and the
  node is its first child, so a parenthesized callee does not count.
- `implicit_decl` (cexpr.c:1724; `csx_sym` at 1748 and 1790) runs only for
  an `is_callee` name. The implicit decl has only REF events, because its
  `s.loc` is the call site, which is not a system location
  (`lazy_event`, csymidx.c:347).
- A `__builtin_*` name without a symbol returns before any event. A field
  callee gets its event at ccall.c:2295 (`csx_field`).
- `strip_paren` exists (check_int.h:613).

**LSP**

- **Call hierarchy** (features.c:1113-1227): `call_item`,
  `lsp_prepare_call_hierarchy` (macros only), `item_macro` (by data id
  and name, else by `selectionRange`), `spelled_ranges` and `lsp_calls`
  (`index_callers` / `index_callees` of the macro graph).
- **Item positions.** `item_macro` converts the item's `selectionRange`
  with `req_loc` (features.c:15), which reads `r->file`, the request's
  document. That is only right because `request_uri` (server.c:1226) takes
  `item.uri` as the request's document. As a result, `handle_request`
  answers null for any item whose file is not an open document
  (server.c:1252), including macro items in headers.
- **Signature help** (features.c:1229-1534):
  - `text_params` splits a declaration's parameter list into collapsed
    texts for a label, so source offsets are lost;
  - `decl_params` reads the definition, then declarations not in a
    `#define` body nor pasted (a system header's lazy event excepted),
    then through the typedef, up to 4 steps;
  - `c_sig` falls back to `cindex_visible` by name.
- **Semantic tokens.** `semantic_data` skips
  `MACRO_BODY | AT_EXPANSION | SYSTEM` events and reads the snapshot's
  file (`r->file->buf`, base `r->file->base`). `r->text` is the editor
  text, which may be newer.
- **Refresh.** `Unit.tok_stale` is set in `handle_request`
  (server.c:1285) when a token answer lacked the unit's own index. The
  builder's publish clears it and calls `send_refresh` (server.c:562,
  871), which sends `workspace/semanticTokens/refresh` only if `S.refresh`
  (the client's `refreshSupport`).
- **Policies.** `REQS` (server.c:1170; `CPolicy` at 1149-1164) maps each method to a `CPolicy`:
  - `CP_NONE`, `CP_ANY`, `CP_POS`, `CP_WAIT`, `CP_SNAP`;
  - `CP_TOK` becomes `CP_ANY` with refresh support, else `CP_WAIT`;
  - call hierarchy is `CP_NONE` today.
- **Requests.** A request without `textDocument` or `item` gets null
  (server.c:1253), so workspace/symbol needs its own path.
- **Units.**
  - Units are made by `unit_named` / `unit_for` and live until exit.
  - `S.docs` holds the open documents, each with its `unit`.
  - didClose reschedules the unit from disk. An "open unit" is therefore
    a unit with at least one `Doc` whose `unit` is it.
- **Capabilities** are written in `initialize` (server.c:1007-1138).
  `callHierarchyProvider` is already true. Code actions are already gated
  on a client capability (`S.code_actions`), so there is precedent for
  capability gating.
- **Kinds.** `C_KINDS[]` (features.c:381) gives the completion kind,
  symbol kind and token type per `CIdxKind` (FUNC 12, OBJ/PARAM 13,
  TYPEDEF 5, ENUMCONST 22, FIELD 8, STRUCT/UNION 23, ENUM 10). Macros use
  `SK_FUNCTION` / `SK_CONSTANT` (features.c:363).
- **Document symbols.** `c_document_symbols` (features.c:494) keeps
  non-REF, non-body events of file-level decls, fields and enumerators,
  and widens entries to their root scope.

**Tests**

- `tests/run.sh` section 14 diffs `tests/symidx/*.expected`, plain and
  with cells. It also runs `tests/symidx/carry` through `--verify-carry`.
- `sym_verify` runs `--verify-symbols` over `tests/check`, `tests/parse`,
  `tests/symidx` and `src`. Any output line other than `symbols:` fails.
- `tests/query/*.cmd` are query goldens; `calls.cmd` covers macro
  callers/callees.
- `tests/lsp_session.py` drives scripted LSP sessions. It strips opaque
  `data` members.
- `bench/tools/symcov.py` runs `--verify-symbols` over the corpus and
  gcc.dg (Round 188: 4,716 files, about 1.02 M events, 0 unindexed).
- Today ` call` would change two goldens with a `func` REF:
  `decls.expected` (a `--dump-symbols` golden) and
  `carry/typedef_var.expected` (a `--verify-carry` difference list).
  Object and parameter REFs that are callees in `b3.expected` and the
  others may change too, plus whatever new cases add.
- With cells, one checker still feeds one builder (`checker_take_index`,
  check.c:772). There is no index merge that `xcalls` would need to join.

## 4. CIX_CALL

### 4.1 Bit, hook, and the case table

`CIX_CALL = 64` in `CIdxFlags`; bit 128 stays free in the frozen index
(the builder borrows it, 4.4). A helper in cexpr_int.h, next to its
`ntag` and `first_child`, because both cexpr.c and ccall.c (the Q7 hook)
include it:

```c
/* Is node i (an identifier or member access) the callee of a call,
 * parentheses aside? */
static inline bool call_callee(const Checker *c, uint32_t i)
{
    uint32_t p = c->par[i];
    while (p != NO_NODE && ntag(c, p) == N_PAREN)
        p = c->par[p];
    return p != NO_NODE && ntag(c, p) == N_CALL &&
           strip_paren(c, first_child(c, p)) == i;
}
```

The four REF hooks pass `CIX_REF | (call_callee(c, i) ? CIX_CALL : 0)`:

- `e_ident`;
- both `csx_sym` calls in `implicit_decl`, which are always calls;
- the field hook at ccall.c:2295, for the `N_MEMBER_EXPR` node (Q7).

Role tests that compare the whole argument must mask it first.
`csx_field` makes the field's lazy DEF event only `if (role == CIX_REF)`
(csymidx.c:521); with `CIX_REF | CIX_CALL` that test would fail and a
called field would lose its declaration event. It becomes
`(role & CIX_ROLE) == CIX_REF`. `csx_sym` passes the role straight to
`push_ev`, and `csx_tag`'s test (csymidx.c:487) never sees the bit.

`is_callee` stays as it is for implicit declarations: gcc rejects
`(f)(1)` with `f` undeclared.

| Case | Event | Flagged | Call-hierarchy edge | Hints |
|---|---|---|---|---|
| `f(x)` | REF at `f` | yes | yes | yes |
| `(f)(x)` | REF at `f` | yes | yes | yes (`)` skipped, 6.4) |
| `(*fp)(x)`, `(&f)(x)` | REF at `fp`/`f` | no: the callee is an `N_UNARY` | no | no (difference 10.2-H6) |
| `fp(x)`, fp an object or parameter | REF obj/param | yes | no: not `CIK_FUNC` (D2, indirect) | yes, through its type |
| `o->cb(x)` | REF field `cb` | yes (Q7) | no | yes, through its type |
| `ID(f(x))`, callee in a macro argument | REF at the spelling, `arg` | yes | yes, site at the spelling | yes |
| `PASTE(g)` forming `g_impl(1)` | REF at the invocation, `expansion` | yes | yes, site at the invocation | no |
| `CALL_LEAF(2)`, callee in a `#define` body | REF at the body, `body` | yes | through `xcalls` (4.4, Q1) | no |
| `#define` written inside a function body | `body` events in the function's range | yes | not from the body event: outgoing skips `body` (5.4); the xcalls of its expansions count | no |
| `__builtin_abs(x)`, no symbol | none (excused in verify) | no | no | no |
| `__builtin___foo_chk` unknown | implicit decl, REF | yes | yes, to an implicit decl | no (no prototype) |
| implicit `undeclared(9)` (`CIDF_IMPLICIT`); also a library function called without its header (`abs(x)`: probed, `func abs external implicit`) | REF only, no DECL | yes | yes; item at the first REF (5.6) | no (no prototype) |
| system function (`strlen`) | REF; lazy `DECL system` event at its declaration | yes | yes; item in the system header | yes, text rule (6.3) |
| `({ int t = f(1); t; })` | REF | yes | yes, caller is the enclosing function | yes |
| `int init = sizeof(f(1));` at file scope | REF | yes (syntactic, as clangd counts it) | yes, caller is the variable `init` (5.1) | yes |
| recursion `f` inside `f` | REF | yes | yes, a self edge (clangd shows it too) | yes |
| K&R definition `int f(a, b) int a, b; {...}`, called | REF | yes | yes | yes: identifier-list events (6.3) |
| call of a K&R/old-style declaration `int f();` | REF | yes | yes | no: the text has no parameters |
| `#define M(x) x(1); g(x)` with `M(f)` | two REFs of `f` at one spelling, one a call | yes, after the OR (4.2) | yes | yes |

Unevaluated calls (`sizeof`, `typeof`, `_Generic` branches not taken) are
flagged. The flag is syntactic, as clangd's references are.

### 4.2 Builder, dedupe, dump, carry, verify

- **Dedupe.** When `csx_finish` drops an event equal to its predecessor
  under `ev_cmp`, it ORs the dropped event's `CIX_CALL` into the kept one
  (`o[-1].flags |= e->flags & CIX_CALL`).
  - Only when the predecessor was written: the loop also skips events
    outside a user or system file (csymidx.c:1111), and then `o[-1]` is
    an unrelated event. The loop keeps a flag for "the last event was
    written".
  - Without this, `M(f)` above would keep one of the two `f` events at
    random, because `qsort` is unstable.
  - `ev_cmp` stays role-only for ordinary events, so readers see one
    event per (loc, decl, role) as today.
- **Dump.** `cindex_print_flags` prints ` call`. `--dump-symbols` and
  `cereal query` C lines gain it.
  - Golden updates: `tests/symidx/decls.expected` (`decls.c:14:22 REF func
    later` gets ` call`), and any REF of an object or parameter used as a
    callee in `b3.expected` and the others.
  - The query goldens are checked in the same step. None changes today:
    their only C reference lines (`query/csym.expected`) are not callees.
- **Carry.** Nothing new for events: flags are copied whole.
- **`--verify-carry`.** `VEv` gains a `call` field, filled by
  `vev_gather`, compared after `role` and printed as ` call` after the
  name. It is not folded into `role`, because the printer indexes role
  names with it.
  - A call bit differing outside the damage is reported like any other
    event difference. None is expected from an edit outside `#define`s,
    because the bit depends only on the syntax around the name. An edited
    `#define` can change call bits and xcalls (4.4) at its invocations
    outside the damage: an effect at a distance, reported, as B3_DESIGN.md
    12.10 accepts for typedefs.
  - The carry goldens print differences, so
    `carry/typedef_var.expected`'s `carried REF func make at
    typedef_var.c:3:23` gains ` call` (`W q = make(1);` is a call).

### 4.3 The runnable invariant check

`--verify-symbols` gains one rule:

> Every `N_CALL` whose callee, parentheses stripped, is an `N_IDENT` (or
> an `N_MEMBER_EXPR`, if Q7 is accepted) that names an entity, is
> presented in user code (`where` succeeds) and is not excused, has a
> `CIX_CALL` event at its token.

Mechanics:

- `push_ev` marks `seen[tok] |= 2` when the role carries `CIX_CALL`. The
  existing unindexed test reads the byte as a truth value, so it is
  unaffected.
- The loop in `csx_unit_end` walks the `N_CALL` nodes.
- A miss prints `FILE:L:C: unindexed 'NAME' (call)` and counts in
  `unindexed`. This keeps the summary line `symcov.py` parses unchanged.
- run.sh's `sym_verify` and `symcov.sh` then check it on the tests,
  cereal's sources and the 4,716-file corpus with no new script.

xcalls are not part of the rule. They are made in the same finish loop
that classifies their events, so "every `body` call event of a function
has an xcall" holds by construction and a check of it would be vacuous. The
`tests/symidx/calls.c` golden and `--verify-carry` cover them instead
(section 9).

### 4.4 Calls written in a `#define` body: `xcalls` (Q1)

D2's grouping is by the scope around the event. A callee spelled in a
`#define` body has its event at the `#define`, not inside the calling
function. So `CALL_LEAF(2)` in `user` would be invisible to both
directions, and macro-heavy C (cereal's audience) would lose most of its
edges.

clangd 20 reports these calls at the invocation: `user` calls `leaf` at
`12:25`, the `CALL_LEAF` name (10.1).

**Design**

- **The array.** `CIndex` gains `xcalls`, `nxcalls`: entries with the
  `CIdxEvent` layout, flags `CIX_REF | CIX_CALL | CIX_AT_EXPANSION`,
  sorted by (file, off), deduped on (loc, decl).
  - Each one sits at the presentation point `p->exp` of a callee token
    that finish classifies `body`.
  - `p->exp` is the same point `where` uses for pasted names, so an xcall
    lands where an `expansion` event of that invocation would.
- **The builder.**
  - `BEv` gains `pres` (builder only), which `where` fills.
  - When the classification loop (csymidx.c:1040) turns a `FROM_MACRO |
    CIX_CALL` event of a `CIK_FUNC` decl into `body`, it appends a copy at
    `pres` with the builder-only bit 128 (`XCALL`). Other kinds get none:
    only call hierarchy reads xcalls, and it takes only functions (E1).
  - `ev_cmp` orders by that bit after the role, so an xcall never merges
    with a real event at the same place.
  - The existing freeze loop converts both in one pass; files are added
    as for any event. At the end the bit-128 entries move to
    `ix->xcalls`.
- **Carry.** The event loop of `cindex_carry` becomes a helper applied to
  `ev` and to `xcalls`.
- **Verify-carry.** It gathers the xcalls into the same VEv list, marked
  by the `call` field of 4.2 (2 for an xcall, printed ` xcall`), not by a
  role value: the printer's `cindex_role_name` masks to the role bits.
- **Dump.** `--dump-symbols` prints them after the events as
  `FILE:L:C xcall func NAME -> #N`.
- **Who reads them.** Only call hierarchy and `query callers|callees`.
  References, rename, highlight, tokens, hover, document symbols and
  hints never see them. That is the reason for a side array instead of
  more events.

**Cost.** About 90 lines of src, tests apart. Memory is one 12-byte entry per
(invocation, callee) of a macro whose body calls a function.

**Alternatives**

1. **xcalls (recommended).** Exact; matches clangd except C5 (risk 2);
   isolated from every other feature. Con: a second array to carry and verify.
2. **Accept the gap.** No code. Con: `CALL_LEAF(2)`, `assert(x)` →
   `__assert_fail` and similar edges are missing in both directions.
3. **Events at the invocation.** Con: they would show up as references,
   rename targets and tokens at the macro name. Every consumer would need
   a new skip.
4. **Compose with the macro index at query time** (body token → its macro
   → that macro's top-level expansions). Con: nested expansions need the
   closure to the outermost source invocation. It also cannot tell which
   expansion produced which decl. More code, less exact.

### 4.5 Invariants

1. `CIX_CALL` is set only on REF events; DECL and DEF never carry it.
2. It is set if and only if the token is the callee of an `N_CALL`,
   parentheses aside, and the name resolved to an entity (4.3 checks the
   "only if" direction by construction and the "if" direction by rule).
3. After dedupe there is at most one event per (loc, decl, role). It is
   flagged if any merged duplicate was.
4. Every xcall has the flags `REF | CALL | AT_EXPANSION`, refers to the
   `CIK_FUNC` decl its body token resolved to, and lies in a file of
   `ix->files`.
5. Carry keeps both arrays' flags. `--verify-carry` compares the bit and
   the xcalls.

## 5. B4a: call hierarchy

### 5.1 Shared queries (csymidx.c, used by the server and `cereal query`)

```c
typedef struct CIdxCall {
    uint32_t other;          /* the caller (incoming) or callee (outgoing)
                                decl; UINT32_MAX: file scope, no named
                                declaration (incoming only) */
    uint32_t other_ev;       /* incoming: the caller's event the 5.1 rule
                                picked; outgoing: cindex_site(callee) */
    uint16_t file;           /* the site */
    uint32_t off;
    uint8_t len;
} CIdxCall;
/* The caller of the site at (fi, off): see 5.1 rule.  UINT32_MAX: none. */
uint32_t cindex_caller_of(const CIndex *ix, uint32_t fi, uint32_t off,
                          uint32_t *ev);
/* Incoming: the CIX_CALL events of decl d (not body) and its xcalls, each
 * with its caller; sorted by (caller name, caller event, site). */
size_t cindex_calls_to(const CIndex *ix, uint32_t d, CIdxCall **out);
/* Outgoing: the CIX_CALL events of CIK_FUNC decls (not body) and the
 * xcalls (all CIK_FUNC, 4.4) inside the root scope holding event ev whose
 * caller (5.1 rule) is ev, with their callees; sorted by (callee name,
 * callee decl, site). */
size_t cindex_calls_in(const CIndex *ix, uint32_t ev, CIdxCall **out);
/* The event that stands for decl d: its DEF not in a #define body, else
 * its first such DECL, else its first DEF/DECL, else its first event. */
uint32_t cindex_site(const CIndex *ix, uint32_t d);
```

**The caller of a site (E3).** Take the root of the site's scope,
`cindex_scope_root(cindex_scope_at(fi, off))`. Then:

1. if the root holds a DEF event of a `CIK_FUNC` decl with
   `scope == 0`, not in a `#define` body, that function is the caller;
2. else the last DEF/DECL event at or before the site, of a `CIK_FUNC` or
   `CIK_OBJ` decl with `scope == 0`, not body, is the caller. This covers
   `int a = f(), b = g();`, where `b` calls `g`;
3. else the first such event in the root;
4. else no caller (`UINT32_MAX`): a `_Static_assert` or an anonymous
   struct at file level, or no root (a declaration spanning files, which
   `freeze_scopes` drops).

Rule 1 ignores position on purpose. A function defined by a macro,
`DEFINE_FN(foo)`, has its root at the invocation, and the body's xcalls
there come before `foo`'s `arg` event.

`scope == 0` keeps parameters and block-scope variables out. A GNU
nested function's calls go to the outer function (parked, 15).

The caller is cached per root within one request, because sites cluster.
Every query is linear in the events of one root or one decl
(`by_decl` CSR). xcalls are not in `by_decl`: `cindex_calls_in` finds a
root's xcalls by binary search (they are sorted by (file, off)), and
`cindex_calls_to` scans them all, which is linear in `nxcalls`, a small
fraction of the events. If that shows in a profile, they get their own
by-decl order at freeze.

### 5.2 Items

A C item, written by `c_call_item(r, decl, ev)`:

- `name`: the decl's name.
- `kind`: `C_KINDS[kind].sk`, which is 12 for a function and 13 for a
  variable caller.
- `detail`: `c_detail`, the first hover line.
- `uri`: the event's file.
- `selectionRange`: the event's name.
- `range`: the event's root scope, widened to contain `selectionRange`.
  If there is no root, `range` equals `selectionRange`. This covers lazy
  system events (no system scopes) and REF-only implicit decls. The LSP
  specification requires `selectionRange` to be contained in `range`.
- `data`: `{"c": DECL, "name": NAME, "doc": URI}`. `doc` is the
  request's document (E4, 5.5).

Macro items keep their shape and gain `data.doc`.

### 5.3 prepareCallHierarchy

1. Resolve the cursor as today. If the macro index answers strongly
   (`TGT_MACRO` with macros, not weak), answer the macro items. This is
   the same precedence as `query_c` (main.c:1329).
2. Else, if `c_file(r) >= 0`, take `cindex_decls_at(cursor)` and answer an
   item for each decl that can be a caller (5.1): `CIK_FUNC`, or
   `CIK_OBJ` with `scope == 0`, at `cindex_site(d)`. clangd 20 also
   answers prepare on a file-level variable, with its initializer's calls
   as outgoing (probe 10.1).
3. Else null.

clangd's item is at the declaration under the cursor (`leaf 2:5` from the
prototype, `18:5` from the definition). Ours is always at the definition
when there is one, so one function has one identity (difference C2).

### 5.4 incomingCalls and outgoingCalls

`lsp_calls` resolves the item (5.5). A macro item takes the existing
path. A C item takes the following.

**Incoming.**

- Take `cindex_calls_to(decl)` and group by `other_ev`, the caller's
  event that the rule picked. Each group is one
  `{from: item(other, other_ev), fromRanges: [sites]}`.
  - Grouping by event rather than decl keeps every group's sites in one
    root and file, which LSP requires of `fromRanges`. In valid C each
    caller has one group anyway (a function's DEF by rule 1, a
    variable's one initialized declarator by rule 2); under error
    recovery (a redefinition) a decl can get two groups, each still
    well-formed.
- Sites without a caller (`UINT32_MAX`) are left out of the LSP answer.
  clangd 20 does the same: a call in a file-level `_Static_assert` is
  not listed (probe 10.1). `cereal query` prints them (5.7).
- A variable item has no incoming calls (calls to it are not edges).

**Outgoing.**

- Take `cindex_calls_in(cindex_site(decl))` and group by callee. Each
  group is `{to: item(callee, cindex_site(callee)), fromRanges: [sites]}`.
- The sites are those in the item's own root that the caller rule (5.1)
  attributes to the item's event, so outgoing is exactly the inverse of
  incoming. For a function that is its whole definition (rule 1). For
  `int a = f(), b = g();` it splits the root: `a` calls `f`, `b` calls
  `g`.
- `body` events in the range are skipped (4.1); xcalls in the range count.
- For a declaration-only function the root is a prototype, so the answer
  is `[]` unless the prototype itself holds a call
  (`int g(int a[sizeof f(1)]);`; clangd's answer here is unprobed).

**Grouping.** Groups are ordered by (name, location) and sites by
position, as clangd orders them.

**fromRanges.** For incoming they are in the caller's file and for
outgoing in the item's file; LSP requires exactly that. Each is the
event's (or xcall's) name range:

- an `arg` event's spelling;
- an `expansion` event's or xcall's invocation name (`len` measured at
  freeze);
- a plain event's name.

### 5.5 Resolving follow-ups, and the `request_uri` fix (E4)

**Today's bug.** `request_uri` treats `item.uri` as the request's
document. A follow-up on any item in an unopened file is answered null:
outgoing of a macro defined in a header, or of a function whose
definition is in a header.

**Fix.**

- `request_uri` prefers `item.data.doc` when it is present, so the
  request runs in the unit that produced the item. Both item writers set
  it. A client that drops `data` falls back to `item.uri`, which is
  today's behaviour.
- `req_loc` gains a file argument (`req_loc_in(r, file, pos)`, with
  `req_loc(r, pos)` calling it with `r->file`). `item_macro` and the C
  resolution then convert `selectionRange` in the item's file,
  `index_find_file(&snap->ix, path(item.uri))`.
- If the doc was closed since, the answer is null, as today for any
  closed document.

**Resolution of a C item, in order:**

1. The decls at the item's `selectionRange` start, in the item's file
   (if that file is not stale), that can be items (5.3) and whose name is
   `data.name` (`item.name` when the client dropped `data`).
2. Else `data.c` if it is below `ndecls` and has that name. This holds
   while the index is the same or carried, which keeps decl ids.
3. Else the `CIK_FUNC` or `CIK_OBJ` decl with `scope == 0` and that name
   that has a DEF event, else the first such decl.
4. Else null.

The item is the client's copy from before any later edit, so neither key
survives every edit: its position is stale once text above it changes,
and a fresh check (`CP_WAIT`, 8) renumbers decls when a declaration is
added before it. Position comes first because it is exact while it
holds. Step 3 is the fallback that survives edits: at file scope a name
denotes one entity per unit, and a block-scope `extern` redeclaration
(also `scope == 0`) has no DEF.

### 5.6 Callees that are unresolved or declared only

- **Declaration only in this unit** (the definition is in another TU or a
  library). The item is the DECL, possibly in a header or a system header
  (lazy event). Outgoing of it is `[]` (but see 5.4 for a call inside
  a prototype). Incoming lists this unit's calls
  only.
- **Implicit decls** (`CIDF_IMPLICIT`, which includes a library function
  such as `abs` called without its header) have no declaration event. The
  item is their first REF, the first call site, with `detail` from hover
  ("func undeclared_fn: int()"). It has the same incoming and outgoing
  rules. (`CIDF_BUILTIN` marks predeclared names such as
  `__builtin_va_list`; no callable function carries it: a `__builtin_*`
  name without a symbol returns before any event, and an unknown
  `__builtin___*_chk` callee becomes an implicit decl, cexpr.c:2039.)
- **Just this TU.** No answer includes another TU's callers or a callee's
  body defined elsewhere. LSP.md will say so, and that B5 lifts it.
- A `__builtin_*` call without a symbol has no event, so it has no edge.
  clangd 20 shows none either.

### 5.7 `cereal query callers|callees` for C names

`mode_query` adds `callers` and `callees` to the kinds that try `query_c`
when the macro index has no strong answer (main.c:1329). `query_c` prints
one line per group. The example below is for the probe file of 10.1, with
Q1 accepted and a file-level `_Static_assert` calling `leaf` added at
line 30 (`_Static_assert(sizeof(leaf(1, 2)) == 4, "");`):

```
== callers calls.c:18:5
calls.c:3:12 func helper: 3:35
calls.c:9:5 obj init: 9:19
calls.c:11:5 func user: 12:25 12:54 13:33 15:21
calls.c (file scope): 30:23
== callees calls.c:11:5
calls.c:6:5 func g_impl: 12:40
calls.c:3:12 func helper: 12:13
calls.c:18:5 func leaf: 12:25 12:54 13:33 15:21
/usr/include/string.h:407:15 func strlen [declaration only]: 14:34
calls.c:14:48 func undeclared_fn [implicit]: 14:48
calls.c:11:5 func user: 16:16
```

- Each line is `LOC KIND NAME[ marks]: SITES`. `LOC` is the caller's
  event that 5.1 picked (callers) or the callee's `cindex_site`
  (callees). `SITES` are `L:C` in the
  site file, which is the caller's file for incoming and the queried
  function's file for outgoing.
- The marks are `[declaration only]` when the callee has a DECL but no
  DEF in this unit, and `[implicit]` for `CIDF_IMPLICIT`.
- Sites without a caller print as `FILE (file scope): SITES`.
- An empty list prints `no callers` / `no callees`, as the macro path
  does.
- The macro path's output is unchanged.

## 6. B4b: parameter-name inlay hints

### 6.1 Request, capability, settings

- **Capability.** `inlayHintProvider: true` is advertised only when the
  client sends `capabilities.textDocument.inlayHint` and
  `initializationOptions.inlayHints` is not `false`.
- **Refresh support.** `S.hint_refresh` comes from
  `capabilities.workspace.inlayHint.refreshSupport`.
- **Response.** `textDocument/inlayHint {textDocument, range}` returns
  `InlayHint[]`: `{position, label: "NAME:", kind: 2, paddingRight: true}`,
  as clangd 20 sends them (it sends label parts; a string is
  equivalent). There is no `inlayHint/resolve`.
- **Turning hints off (Q5).**
  1. **Clients' own toggles plus `initializationOptions.inlayHints:
     false` (recommended).** VS Code `editor.inlayHints.enabled`, Neovim
     `vim.lsp.inlay_hint.enable`; about 8 lines. Con: changing it needs a
     restart.
  2. **Clients' toggles only.** No code. Con: a client without a toggle
     cannot turn them off.
  3. **`workspace/didChangeConfiguration`.** Live. Con: the first
     configuration handling in the server (~60 lines). Parked.

### 6.2 Which calls

The answer covers hints whose position lies in `[range.start,
range.end]`. That is clangd's filter: a range holding only the line
`r += two(` gets no hints for arguments on the next lines (probe 10.1).

**The walk.**

- `fi = c_file(r)`; if it is below 0, answer `[]`.
- Start at the root scope of `range.start` (or at `range.start` outside
  every root), because a call that begins before the range can have
  arguments inside it.
- Walk events from `cindex_lower(fi, start)` while `off <= range.end`.
- Take `CIX_CALL` events without `MACRO_BODY | AT_EXPANSION | SYSTEM`.
  These are the names spelled in this file's text, source or macro
  argument, the same filter as semantic tokens. xcalls are never read.

**Cost.** Linear in the root's events plus one declaration scan per
distinct callee. The callee's names are cached per request in a small
decl → names map.

### 6.3 Parameter names (E5)

**Spans.** `text_params` and `skip_blank` move from features.c to
csymidx.c, so that `--verify-symbols` can run the same code. Their
output struct moves with them: `Sig` (features.c:1235) is `label`,
`offs` and `variadic`, all common types, plus the LSP-only `doc`, which
stays in a wrapper in features.c. `text_params`
also gains an optional output: each parameter's
source span `[begin, end)` in the declaration's file, with surrounding
blanks and comments trimmed. The collapsed label is unchanged.
`decl_params` passes the spans on, so hints use the same declaration
signature help shows: the definition, then declarations, then the
typedef.

**The name of span i:**

1. **From the index.** The first DECL or DEF event of a `CIK_PARAM` decl
   in the span whose innermost scope (`cindex_scope_at`) begins at or
   before the span's start. If there is none, the first such REF event:
   K&R identifier lists name their parameters with REFs.
   - The scope test skips names of a nested prototype: in
     `int (*cb)(int depth)` the outer name is `cb`, not `depth`.
   - Preferring declarations skips a use of an earlier parameter:
     `int f(int n, __typeof__(n) x)` names `x`, not `n`.
   - An unnamed parameter or `...` has no event, so no name.
2. **From the text, only when the declaration's event is `CIX_SYSTEM`.**
   System headers have no parameter events (3), and clangd shows
   `dest: src: n:` for `memcpy` and `s:` for `strlen`.
   - If the span has a parenthesized declarator, `( * NAME )`, the name
     is the first identifier after the stars and qualifiers inside it.
   - Otherwise the name is the last identifier at depth 0 that is not a
     keyword, skipping `__attribute__((...))` and GNU spellings
     (`__restrict`, `__restrict__`, `__const`, `__extension__`,
     `_Nonnull`, `_Nullable`). It also needs a type-specifier token
     before it: a keyword type, an identifier, or a tag after
     `struct`/`union`/`enum` (the tag itself excluded).
   - `FILE *`, `size_t` and `const char *` alone have no name.

Then **leading underscores are stripped** (`__dest` becomes `dest`, as
clangd does). A name that is empty after that gives no hint.

The index route is exact on user code, where a wrong hint is most
visible. The text rule only fills the one place the index has nothing
(Q3).

**The text rule's check.** `--verify-symbols` runs the text rule on every
user DEF/DECL of a function whose `text_params` succeeds, and compares it
with the index names.

- It skips spans holding macro-expanded tokens, K&R lists, and
  declarations whose event is `body` or `expansion`.
- A disagreement prints `FILE:L:C: param name 'TEXT' vs 'INDEX'` and
  counts in a new `param misses` field of the summary line. `symcov.py`'s
  regex gains the field.
- run.sh gates on zero for its four trees.
- symcov reports the corpus count as a measurement. A rule change is
  judged by that count going down.

### 6.4 Arguments: re-lexing the snapshot text

The argument list is read from `r->file->buf`, the snapshot's text that
the event offsets refer to, not from `r->text`. The `CP_VIEW` policy
(8) first waits up to 1.5 s for the newest edit's snapshot, so the two
usually agree. A carried index has already dropped the events overlapping
the edit.

**The lexer.** From the end of the callee name:

- skip blanks, comments and `)` (a parenthesized callee written in the
  source), then require `(`. Anything else means no hints for this call,
  for example `APPLY(two)` whose parentheses come from a macro body;
- split at commas at depth 0 of `()[]{}`, skipping string and character
  literals (with escapes) and comments, up to the matching `)`;
- give up on a `#` at the start of a line inside the list, on end of file,
  or after 64 KiB;
- each argument's position is its first character that is not blank or a
  comment.

clangd places hints there too: `two(ARG(23), 24)` gets `count:` at `ARG`.

**Matching.** Argument i is matched with parameter i while
`i < nparams` and parameter i is not `...`. Extra arguments, those of a
variadic function or of an erroneous call, get none.

### 6.5 Suppression rules

Checked against clangd 20 by probe (10.1). "Same" means the probe agreed.

| Rule | Ours | clangd 20 |
|---|---|---|
| Argument, outer parentheses stripped, is a single identifier equal to the name: the argument and the parameter name each with leading underscores stripped (section 18), case-sensitive | no hint | same: `two(count, flag)`, `two(count + 1, (flag))` hint only the first. Second probe: `two(Count, FLAG)` hints both (case-sensitive); `under(count, flag)` for `(int __count, int _flag)` hints none, `under(count, _flag)` hints `flag:` (**wrong**, section 18: the argument's leading underscores are stripped too, so `_flag` for `_flag` is suppressed); `trail(count, flag)` for `(int count_, int flag)` hints `count_:` (trailing underscores kept) |
| `/*name*/` or `/*name=*/` (blanks allowed) right before the argument | no hint | same: `two(/*count=*/1, /*flag*/2)` |
| Member access equal to the name (`o->count`) | hint | same: hinted |
| Single parameter, setter: the function name minus `set` (case-insensitive) and leading `_` equals the parameter name, ignoring case | no hint | same: `setLimit(4)`, `set_limit(1)`, `setCount(1)` and `set_Count(2)` for `(int COUNT)` suppressed; `setup(2)` gets `mode:`, `set_(1)` gets `x:` |
| Other single-parameter functions | hint (`value:`, `p:`, `s:`) | same |
| Unnamed parameter | no hint | same |
| `...` and the arguments it takes | no hint | same: `vari("%d %d", 9, 10)` gets only `fmt:` |
| No prototype (`int knr();`, implicit decls) | no hint | same |
| K&R definition `knrdef(a, b) int a, b;` | `a:`, `b:` | same |
| Function pointer through a typedef (`cbt fp; fp(15, 16)`, `o->t(18, 19)`) | `width:`, `height:` | same |
| Field declared as `int (*cb)(int depth)` | `depth:` | none (difference H3) |
| `__builtin_*` | none (no event) | same |
| Callee name spelled in a `#define` body (`TWO(20)`) | none | `count:` at `20` (difference H1) |
| Argument is a macro expanding to the name (`two(CNT, 1)`, `CNT` → `count`) | `count:` | suppressed (difference H2) |
| Leading underscores (`under(int __a, int _b)`) | `a:`, `b:` | same |
| `(*fp)(x)` | none (not flagged) | unprobed (H6) |

### 6.6 Refresh

`Unit.tok_stale` generalizes to `Unit.views_stale`, a bit set:
`VIEW_TOKENS` = 1, `VIEW_HINTS` = 2.

- **Policy.** `CP_TOK` becomes `CP_VIEW`. The `REQS` row carries its view
  bit, and the policy is `CP_ANY` when the client takes that view's
  refresh, else `CP_WAIT`.
- **Marking.** `handle_request` sets the view's bit under the same
  condition as today's token rule (server.c:1285): the answer lacked the
  newest snapshot's own index.
- **Publishing.** The builder's publish calls `send_refresh(bits)`, which
  sends `workspace/semanticTokens/refresh` and/or
  `workspace/inlayHint/refresh`, each only if the client supports it, and
  clears the bits.

This is one mechanism for both views and about 15 changed lines.

## 7. B4c: workspace/symbol

**Handler.** `workspace/symbol` is matched before `REQS`, because it has
no document.

1. Under `S.m`, for each unit with an open document (`S.docs`, each unit
   once), take `snapshot_ref(u->snap)` and `cindex_ref(snap->cidx)`, the
   unit's own or carried index, whatever is there.
   - There is no wait and no check is scheduled (E6, D4).
   - A unit with no snapshot yet contributes nothing. A unit with a
     snapshot but no index, because its first check is pending or it is
     over the size limit, contributes its macros only.
2. Release the lock and collect candidates (below).
3. Release the references.

`workspaceSymbolProvider: true` is always advertised.

**Candidates.** Workspace symbols are the flattened document symbols of
the units' user files, so the selection is shared rather than restated:

- `c_document_symbols`' event filter becomes
  `c_symbol_event(ix, fi, k)`, used by both. It keeps file-level
  declarations, plus fields and enumerators, in user files, not
  `body`/`system`, files not stale.
- For each decl, one entry per unit, at its DEF if any, else its first
  kept DECL. `containerName` is the parent's name for fields and
  enumerators (`""` for an anonymous parent), else `""`.
- Macros: `pp->macros` whose `name_loc` is a `real_loc` in a user file
  (not system, not predefined or from the command line, not aliases), as
  document symbols lists them.
- Kinds: `C_KINDS[].sk`; macros `SK_FUNCTION` / `SK_CONSTANT`. clangd uses
  15 (String) for macros (difference W3).
- Location: the name's range in that snapshot's line tables.
- Dedupe across units by (path, offset, name): a header seen by two units
  is listed once.

**Matching** (a port of clangd 20.1.8's behavior, as probed; the history of
the rule is in section 19):

- Case-insensitive; an empty query matches everything.
- Character types: lower (`a-z`, digits, bytes >= 0x80), upper, other. A
  digit is lower case, so **no segment starts at a letter-digit change**
  (`a` does not match `IPv4addr`, `u8` matches nothing in `utf8_decode`).
- A head is index 0, a character after an "other", a lower-to-upper change,
  or the last capital of a capital run that is followed by a lower-case
  letter (`TString`: the `S`).
- The query's characters must appear in order; the first must match at a
  head; each later one matches right after the previous match or at the
  next head after it (no word is skipped).
- clangd has two matchers. Symbols of the open documents (its in-memory
  index) use this fuzzy match alone. Symbols of included headers (its
  Dex index) must also pass a trigram test: the query's tokens (the query
  itself when it has under 3 characters, else every window of 3 raw
  characters) must each be among the name's tokens, which are built from
  index 0, the first two heads and every non-separator. `ws_match(q, name,
  plain)` takes `plain` for the first kind; `lsp_workspace_symbols` gets the
  open documents' paths to choose.
- Probed with 95,758 query/name pairs (open-file symbols and macros: 0
  differences; header symbols: 3, the residual in section 19).

**Ranking:**

1. exact (case-sensitive);
2. exact ignoring case;
3. prefix;
4. other matches by fewer jumps (a non-adjacent match; the first is free);

then C declarations before macros, a shorter name, the name, the path and
the offset.

**Limit:** the first 100, clangd's default.

**Response.** `SymbolInformation[]`, `{name, kind, location,
containerName}`, the form every client accepts.

**Cost.** Linear in the user-file events of the open units, a few ms for
large units, with no allocation beyond the candidate list. Memory is only
the references held during the request (E6).

**Partial, documented (D4).** LSP.md will state:

- only units with an open document are searched;
- a unit still checking contributes its last or carried index;
- headers count when a searched unit includes them;
- system headers are left out (Q6);
- B5's project index removes the first limit.

## 8. Request policies

| Request | Policy | Why |
|---|---|---|
| prepareCallHierarchy | `CP_POS` | Resolves a position, like definition: the carried index answers unless the cursor is in its damage. A macro answer needs only the snapshot, which every policy but `CP_NONE` waits for. |
| incomingCalls, outgoingCalls | `CP_WAIT` | Lists the user acts on, like references. A carried index silently lacks the calls in the damage, which is exactly where the user is editing. Macro items wait too (≤ 1.5 s while a check runs), since the policy is per method. Making it per item would be a special case for an at most 1.5 s gain. |
| inlayHint | `CP_VIEW` (hints) | A view the server can refresh: answer from the carried index at once, then refresh after the check; without refresh support, wait as tokens do. |
| workspace/symbol | its own handler, no wait (E6) | Many units; waiting up to 1.5 s for each, or starting checks, would cost more than partial data, which D4 accepts anyway. |
| semantic tokens | `CP_VIEW` (tokens) | Unchanged behaviour, renamed. |

## 9. Tests per slice

**B4a**

- **`tests/symidx/calls.c` + `.expected` (new).** Every row of 4.1, plus a
  function defined by a macro, the `M(f)` dedupe case, a called field
  (its lazy DEF event must remain, 4.1) and the nested shapes of risk 2
  (`OUTER()`, `ID(CALL_LEAF(2))`). run.sh runs it
  plain and with cells, so the cells path must produce the same flags and
  xcalls.
- **Golden updates.** `decls.expected` (` call`) and any other that gains
  ` call`, checked line by line, not blindly regenerated.
- **`tests/symidx/carry/calls.{c,old}` (new).** An edit outside the calls,
  and one that turns `f;` into `f(1);` inside the damage.
  `--verify-carry` reports no difference.
- **`--verify-symbols` call rule (4.3).** Runs through run.sh section 14
  on four trees. `symcov.sh` on the corpus is run once per slice and its
  line recorded in the results.
- **`tests/query/ccalls.cmd` (new, with `.c`, `.h`, `.expected`).** The
  5.7 output for: callers and callees of a function; a system callee;
  implicit; declaration-only; file scope; macro-body (xcall); recursion;
  a variable caller; a name that is a strong macro (the macro output
  stands).
- **`tests/lsp/ccalls.json` (new).** The harness cannot feed a response
  back into a request, so follow-up items are hand-written, as
  `basic.json`'s macro items are; they carry `data: {"name", "doc"}`
  (no `c`, which is not stable to write by hand) unless stated.
  - prepare on a C function, on a macro, on a file-level variable (an
    item), on a local variable (null), off any name (null);
  - `int a = f(), b = g();`: outgoing of `a` is `f` only, of `b` `g`
    only;
  - incoming and outgoing, with grouping and fromRanges;
  - an item defined in an **unopened header**. This fails today and
    passes with E4; the macro version of it is in the same scenario;
  - a follow-up after lines are inserted above the function, with the
    item's pre-edit positions: the name step (5.5, step 3) finds it;
  - a follow-up with `data` stripped by the client. The session script
    strips `data` from the transcript only; the scenario hand-writes an
    item without `data` to check the fallback.

**B4b**

- **`tests/lsp/hints.json` (new).**
  - one range over a file with every row of 6.5;
  - a range covering only the first line of a multi-line call (no hints);
  - a client without the capability: the recorded initialize answer has
    no `inlayHintProvider`, and `absent` shows no
    `workspace/inlayHint/refresh`;
  - `initializationOptions.inlayHints: false`;
  - refresh: `cereal/holdChecks`, an edit, a hint request answered from
    the carried index, release, then `wait` for
    `workspace/inlayHint/refresh`;
  - the same with semantic tokens, to check the bit set: one refresh each,
    no duplicate.
- **Signature help goldens** (`csym_b3`) must stay unchanged. They share
  `text_params`.
- **Param-name check (6.3)** through `sym_verify`; symcov reports the
  corpus count.

**B4c**

- **`tests/lsp/wsym.json` (new).**
  - two units open that share a header (dedupe);
  - queries `""`, exact, prefix, segment (`lea`), non-match (`lf`), and a
    field (`containerName`);
  - macros, with system ones absent;
  - a unit whose first check is held (`holdChecks`): its macros only,
    and no wait. The response arrives while the check is held;
  - a document closed, which drops its unit unless another open document
    is in it;
  - the 100 limit, from a `write` step generating 150 declarations.

**Each slice** updates LSP.md (Capabilities, C symbol index flags, Tests),
ROADMAP and HISTORY.

## 10. The clangd differential

### 10.1 Probe (2026-10-10, clangd 20.1.8; no compile database, the second probe a `compile_flags.txt` with `-std=c11`)

These are scratch files, deleted after the probe. Their cases become the
diff corpus (10.2).

- **Call hierarchy.**
  - incoming of `leaf` lists `fp` (variable, `int (*fp)(int, int) =
    leaf;`, a non-call reference), `helper`, `init` (variable, `sizeof`
    at file scope) and `user` at `12:25 12:54 13:33 15:21`. `12:25` is
    the `CALL_LEAF` invocation of a body-spelled call.
  - Outgoing of `user`: `g_impl` (12:40, pasted), `helper`, `leaf`,
    `strlen` (an item in the system header), and `user` itself. There is
    no `fp`, `o->cb`, `__builtin_abs` or `undeclared_fn` (C99: an error
    in clang 20, so no decl).
  - prepare on the prototype gives an item at the prototype.
  - Second probe: for `int a = sizeof(leaf(3, 4)), b = sizeof(leaf(5,
    6)) + ...;` incoming of `leaf` gives two groups, `a` and `b`, each
    with its own sites, and prepare on `a` or `b` gives a variable item
    whose outgoing is its own initializer's calls. A call inside a
    file-level `_Static_assert` is not listed at all.
  - Nested macros: `OUTER()` (body `CALL_LEAF(1)`) is sited at `OUTER`;
    `ID(CALL_LEAF(2))` and `ID(P(g))` at the inner name (C5).
- **Inlay hints.** Each row of 6.5 marked "same" was observed, the
  case, underscore and setter details in a second probe. Labels
  are `NAME:`, kind 2, `paddingRight: true`. The range filter is by hint
  position.
- **workspace/symbol.**
  - `lea` gives `leaf` (12), `CALL_LEAF` (15), and `__LEAF` and
    `__LEAF_ATTR` (15, glibc macros);
  - `lf` gives nothing;
  - `""` gives 100 results, mostly glibc functions;
  - `containerName` is `""`; a non-standard `score` is included.

### 10.2 Script and intended differences

**The script.** `bench/tools/clangd_diff.py` (with a `clangd_diff.sh`
wrapper like `symcov.sh`: `nice`, `</dev/null`).

- **Inputs.** It runs `clangd` and `cereal lsp` on the hand-picked corpus
  `bench/clangd_diff/`. That holds the probe's cases plus three or four
  real files listed by path from `~/corpus` (a macro-heavy one, a
  K&R-era one, one with function-pointer tables), and a `.cereal` and
  `compile_flags.txt` with the same flags.
- **Requests.** For each file: inlayHint over the whole file;
  prepare/incoming/outgoing at every function definition's name and
  every file-level variable's;
  workspace/symbol for a fixed query list.
- **Normalization.** It drops `detail`, `data`, `score`, label-part
  structure and `range` (keeps `selectionRange`), and sorts.
- **Output.** It prints each difference with the id of the intended
  difference it matches, or `UNEXPECTED`. The exit status is 1 on any
  unexpected difference.
- **Where it runs.** Not in run.sh, because clangd may be absent. It runs
  once per slice; the result goes in the review log.

The intended differences, each kept as a pattern in the script:

| Id | Difference | Why |
|---|---|---|
| C1 | Incoming lists calls only; clangd lists every reference (`fp = leaf`, `&f` passed as a callback) | D2 |
| C2 | prepare's item is at the definition even from a prototype | One identity per function (5.3) |
| C3 | Implicitly declared functions have edges (cereal accepts C89 implicit declarations with a pedwarn; clang 20 errors) | The checker's dialect |
| C4 | Without Q1, body-spelled calls are missing (with Q1, none expected) | 4.4 |
| C5 | A call formed inside a macro argument (`ID(P(g))`, `ID(CALL_LEAF(2))`) is sited at the outer invocation `ID`; clangd sites it at the inner name | B2's `p->exp` (risk 2) |
| H1 | No hints for calls whose callee is spelled in a `#define` body | Only names spelled in this file's text are re-lexed (6.2) |
| H2 | An argument macro expanding to the parameter name still gets a hint | Text re-lex sees the macro name |
| H3 | Hints for a field declared with an in-place function-pointer declarator (`int (*cb)(int depth)`) | Our names come from the declaration's text; clangd's from the type loc it finds |
| H4 | Declaration choice when the prototype and definition name parameters differently: ours follows signature help (definition first) | Consistency with signature help (6.3); clangd's choice to be recorded from the diff |
| H5 | Text rule vs clangd for system headers whose parameter lists use macros | Heuristic (6.3); counted, not hidden |
| H6 | `(*fp)(x)`, `(&f)(x)`: no hints | Not flagged (4.1) |
| W1 | System-header symbols left out | Q6 |
| W2 | Only units with an open document (clangd also indexes its preambles' symbols) | D4 |
| W3 | Macro kind 12/14 instead of 15 | Same as document symbols |
| W4 | Ranking ties ordered differently | Different scoring; the set of matches must agree |

## 11. B5 forward compatibility and the spike

B4 must not preclude the following.

- **Per-TU summaries.**
  - The call flag lives in event flags, and xcalls and roots live in
    `CIndex`.
  - None of B4's C answers reads the snapshot's macro index (only the
    macro precedence does).
  - So a per-TU on-disk summary can serialize `ev`, `xcalls` and `scopes`
    and answer callers, callees and symbols without re-checking.
- **Caller triples.** Caller attribution (5.1) needs only the frozen
  index. A summary can precompute (caller, callee, site) triples keyed by
  name and linkage (static names also by file).
- **Item identity.** Item `data` is an object resolved by position first.
  B5 can add a cross-TU key, such as `{"usr": name + linkage [+ file]}`,
  without changing the order. Nothing client-visible depends on decl ids
  alone.
- **workspace/symbol as a source list.** Its candidate list is built per
  source (one per unit). B5 adds the project summaries as another source,
  with open units overriding their summary. Dedupe is already by
  (path, offset, name), not by unit.
- **The spike.** A 1-hour spike on the checker's static state comes
  before the B5 design. The builder thread alone may check today
  (`DeferredReq`, server.c: "the checker keeping static state"). The
  spike answers which state is static, and whether B5's background
  indexing can run checks concurrently in-process or needs worker
  processes. Its answer shapes the B5 design, so nothing in B4 depends on
  it.

**Resolved by B5_DESIGN.md (2026-10-12).** The spike answered (B5 D1:
hybrid, in-process first). Item `data` gains `"key"` (D8's identity key,
statics by file and offset) rather than `usr`. Project data is unioned
with open units, not overridden by them (B5 E11).

## 12. Slices, order and estimates

The ROADMAP says 400 lines for B4. Past rounds ran 2 to 3 times over
(B3: 1,056 against 550). The ranges below are lines added in src plus
tests, low to high, and the high end is the honest planning figure.

| Slice | Content | src | tests and goldens | Total |
|---|---|---|---|---|
| B4a | `CIX_CALL` hooks and dedupe (20), print and verify-carry (10), verify rule (40), xcalls (90), shared queries 5.1 (120), LSP items, prepare, calls and E4 (150), query output (60) | 490 - 900 | 200 - 400 | 690 - 1,300 |
| B4b | spans in `text_params` (40), names from the index and the text rule (90), re-lex and suppressions (90), handler (60), capability, options and the view-bit refresh (40), param-name check (40) | 360 - 700 | 120 - 250 | 480 - 950 |
| B4c | handler and unit walk (60), shared symbol filter (20), matcher and ranking (70), candidates, dedupe and JSON (70) | 220 - 450 | 80 - 160 | 300 - 610 |
| clangd_diff | script and corpus | 150 - 250 | | 150 - 250 |
| **B4** | | 1,220 - 2,300 | 400 - 810 | **1,620 - 3,110** |

**Order:** B4a, then B4b (it reads `CIX_CALL`), then B4c (independent;
it could go first if the user wants it early). Each slice ends with:

- run.sh green;
- symcov on the corpus;
- clangd_diff with no `UNEXPECTED`;
- a results section appended here.

## 13. Risks

1. **Overrun.** The estimate is 4 to 8 times the ROADMAP's 400. If B4a
   alone passes 1,300 lines, stop and re-plan B4b and B4c.
2. **`p->exp` in nested expansions.** Read in pp.c:160-167, 961 and
   ppexpand.c:1538: a token from a macro context gets the context's
   `exp_loc` ("outermost call site"), and a nested expansion inherits the
   `tok_exp_loc` of its name token. Probed on 2026-10-10 with pasted
   names, whose `expansion` events show `p->exp` (body events do not):
   - `OUT2()` whose body is `P(g)` → `g_impl` at `OUT2`. clangd 20 puts
     the call there too.
   - `ID(P(g))` → at `ID`; clangd 20 puts it at `P`, inside the argument.

   The `self_loc` rule (a source-spelled token in argument pre-expansion
   is its own expansion point) did not apply here. So xcalls, which use
   the same `p->exp`, are expected at `OUTER` for `OUTER()` (clangd
   agrees) and at `ID` for `ID(CALL_LEAF(2))` (clangd: at `CALL_LEAF`).
   This is difference C5 (10.2). It is inherited from B2's placement of
   `expansion` events, so changing it is a preprocessor change outside
   B4. `calls.c` pins both shapes.
3. **Missing roots.** Some sites get no caller: they are invisible in LSP
   and "(file scope)" in query. Causes:
   - a declaration spanning files, or error recovery;
   - a file `#include`d inside a function body. Its sites have no scope
     of their file; the root lies in the including file.
4. **Re-lex mismatch.** A callee from a macro argument followed in the
   source by `(` that is not its call (`CALL2(two)(junk)`) would get wrong
   hints. This needs pathological code; requiring `(` limits it.
5. **The text rule on system headers.** glibc spells
   `__restrict __dest`. Other libcs (musl, BSD) may put macros inside
   parameter lists. H5 counts these differences; the worst case is no
   hint or an odd one for a libc function.
6. **Concurrent edits to server.c.** Another agent's work on
   `src/lsp/server.c` landed as 809a6c8 (rename checks on the builder
   via `DeferredReq`) while this doc was written; the cited lines were
   rechecked against it. B4's server changes (`REQS`, `request_uri`,
   `views_stale`, the workspace/symbol path) must still be made against
   whatever is current, not this doc's line numbers.
7. **Golden churn.** ` call` touches existing goldens. Each changed line
   is checked to be a call.
8. **Clients.** `selectionRange ⊆ range` is enforced (5.2). A client that
   keeps `data` out of follow-ups gets the position and name fallbacks
   (5.5).

## 14. Open questions for the user

**Q1. Calls written in a `#define` body** (4.4, extends D2).

1. **xcalls side array (recommended).** Exact; matches clangd except
   for calls formed inside a macro argument (C5). Con: about 90 lines, a
   second array to carry.
2. **Accept the gap.** Con: `assert` → `__assert_fail` and every
   `CALL_LEAF`-style edge is missing.
3. **Events at the invocation.** Con: they pollute references, rename and
   tokens.
4. **Compose with the macro index at query time.** Con: more code, inexact
   for nested macros.

**Q2. Which units workspace/symbol searches** (D4).

1. **Units with an open document (recommended; D4 literally).** Con: a
   closed file's symbols vanish even though its unit is still in memory.
2. **Every unit with a snapshot.** Con: units of closed documents keep
   being rebuilt from disk, which is surprising scope creep before B5.

**Q3. Parameter names of system-header callees** (6.3).

1. **Index names, plus a text rule for system declarations only
   (recommended).** Con: a heuristic, measured by the param-name check.
2. **The text rule everywhere,** with the index as the test oracle only.
   Con: less exact on user code.
3. **Index only.** Con: no hints for `memcpy`, `strlen` and the like.

**Q4. The `request_uri` fix also changes macro items** (E4). Recommended:
yes. It is a bug today: follow-ups for header items answer null.

**Q5. Turning hints off** (6.1). Recommended: option 1, client toggles
plus `initializationOptions.inlayHints: false`.

**Q6. System headers in workspace/symbol** (7).

1. **Leave them out (recommended).** "Workspace" means the user's code.
2. **Include them,** as clangd does. Con: `""` and short queries would
   list glibc first.

**Q7. Flag field callees `CIX_CALL`** (4.1). Strictly, D2 names the hook in
cexpr.c. Recommended: yes. It creates no edges and is required for
`o->t(18, 19)` hints, which clangd shows. It brings the `csx_field` role
mask (4.1) with it.

## 15. Parked

Moved to ../PARKED.md (call hierarchy and hints).

## 16. Review log

Written 2026-10-10 against the Windows checkout at 82c44b2 (rounds 0 to
4) and 809a6c8 (rounds 5 on), read-only; nothing built. Each round
re-read the cited code, not memory of it.

**0. Found while drafting** (checked against code or clangd before the
text was written):

- clangd in WSL is 20.1.8, not 18. Its outgoing calls work, so 10.1
  probes 20.1.8.
- `implicit_decl` makes no DECL event, so the item for an implicit
  function is at its first REF (5.6).
- `req_loc` reads `r->file`. The E4 fix therefore needs `req_loc_in`,
  or header items would convert positions with the `.c` file's lines.
- workspace/symbol cannot use `CP_ANY` without waiting per unit; the
  deviation is stated as E6.
- The caller rule had to handle several declarators (`int a = f(), b =
  g();` attributes `g` to `b`) and functions defined by a macro (rule 1
  ignores position).
- The hint walk starts at the root containing `range.start`; starting at
  `range.start` would miss calls whose name is before it.

**1. Claims against the code.** Found and fixed:

- `abs(x)` without its header is `CIDF_IMPLICIT`, not `CIDF_BUILTIN`; a
  `[builtin]` mark was removed from 5.7.
- The verify-carry printer indexes `role_names` by the flags byte, so
  ` call` is a separate field (4.2). `typedef_var` is a verify-carry
  golden whose difference line gains ` call`.
- `(*fp)(x)`'s callee is an `N_UNARY`, so it is not flagged (4.1, H6).
- A claim about how VS Code treats `selectionRange` outside `range` was
  replaced by the LSP specification's requirement (5.2).
- An assumption that clangd gives no container for file-scope callers
  was wrong: it lists variables (`fp`, `init`) as callers (10.1).
- The cells path has no index merge step to change; the text said it
  had.

**2. Edge cases and contradictions.** Found and fixed:

- Incoming calls are grouped by the caller's event, not its decl, so each
  group's `fromRanges` lie in one root and file, as LSP requires (5.4).
- A file `#include`d inside a function body yields sites with no caller
  (risk 3).
- workspace/symbol's macro candidates must have a `real_loc` `name_loc`
  in a user file; macros from the command line or built-ins have none
  (7).
- A client that drops `data` falls back to position resolution (5.5).
- The no-capability test's wording was corrected to what the recorded
  transcript can show: no `inlayHintProvider`, no refresh request (9).
- E1 and Q7 disagreed on whether field callees are flagged; E1 now
  defers to Q7.
- `text_params` moves to csymidx.c so hints and signature help share it;
  the signature help goldens are listed as unchanged (9).

**3. Self-consistency.** Found and fixed:

- A planned "every body call has an xcall" check was vacuous, since the
  same loop makes both; it was removed (4.3).
- Invariant 4 in 4.5 (an xcall's flags, decl and file) corrected to
  match 4.4.
- A stale comment on `other_ev` in 5.1.
- The 5.7 query example had wrong columns (`14:48`, `30:23`) and
  unlabelled lines.
- The estimates' arithmetic was rechecked: the component sums give 490,
  360 and 220, and the columns give 1,220 to 2,300 and 400 to 810, so
  1,630 to 3,110.

**4. Full read of 1 to 16.** Found and fixed:

- Parameter names (6.3) took the first event of any role, so
  `int f(int n, __typeof__(n) x)` would name `x` as `n`. Declarations
  now come first; REFs only for K&R lists.
- The setter suppression in 6.5 was marked as unverified (round 5 then
  probed it).
- This log had been sketched before the rounds ran; it was rewritten
  from what each round found.

**5. Every code reference re-read at 809a6c8, plus three clangd probes.**
HEAD had moved from 82c44b2 to 809a6c8 (server.c, features.c, lsp.h).
Found and fixed:

- **A real bug in the hook plan.** `csx_field` makes a field's lazy DEF
  event only `if (role == CIX_REF)` (csymidx.c:521). Passing
  `CIX_REF | CIX_CALL` (Q7) would have silently dropped it. The test is
  now masked (4.1), and `calls.c` checks it.
- **The dedupe OR** could hit an unrelated event, because the freeze loop
  also skips events outside user and system files (csymidx.c:1111). It is
  now only applied after a written predecessor (4.2).
- **Outgoing of a variable** took its whole root, so in
  `int a = f(), b = g();` the item `b` would have called `f`. Outgoing now
  filters by the caller rule, making it the inverse of incoming. A
  clangd probe gives exactly that split.
- **prepare on a file-level variable** answers an item in clangd 20. Ours
  answered null although variable items already exist as callers. It now
  answers (5.3, test updated).
- **The `_Static_assert` assumption** (clangd lists no container-less
  calls) is now probed and true (5.4).
- **Case-sensitivity** (6.5) is probed. Arguments match case-sensitively
  and verbatim, and only the parameter's leading underscores are
  stripped (`count_:` keeps its trailing one). The setter rule is
  case-insensitive (`set_Count(int COUNT)` suppressed, `set_(1)` hinted).
- **Nested `p->exp`** (risk 2) was read in pp.c and ppexpand.c and
  probed through pasted names. `OUTER()` matches clangd. `ID(P(g))` is
  sited at `ID`, where clangd uses `P`; xcalls inherit this. It is
  recorded as intended difference C5.
- **Stale line numbers.**
  - `lazy_event` is at 347, not 348.
  - `e_ident`'s call is at 2060.
  - `REQS` is at 1170.
  - LSP.md's placement line is at 186-188.
  - The baseline is now 809a6c8. Risk 6 notes that 809a6c8 landed.
- **Test list.** `calls.c` gained the field and nested cases. The
  ccalls scenario gained the variable cases.

Confirmed unchanged:

- csymidx.c 311, 336, 576-611, 671, 717, 908-980, 1040, 1093, 1581,
  1773 and 1890;
- csymidx.h:58 and check_int.h:613;
- cexpr.c 1689, 1724, 1748 and 1790; ccall.c:2295; check.c:772;
- main.c 1245 and 1329; crename.c:281;
- features.c 15, 363, 381, 494, 1113-1227 and 1229-1534;
- server.c 54 (`DeferredReq`'s static-state comment), 562, 871,
  1007-1138, 1226, 1252 and 1285;
- the goldens `decls.c:14:22` and `typedef_var.c:3:23`;
- the harness steps `absent`, `wait` (with `uri: null`), `write` and
  `cereal/holdChecks`.

Q1 to Q7 were checked against sections 4 to 9. Only Q7 needed a note (the mask).
The estimate arithmetic still holds. The round 5 changes are a few lines
each, inside the B4a ranges.

**6. Read of 4.4 to 6.2 and 10 to 14 after round 5.** Found and fixed:

- Q1 and 4.4 still said xcalls "match clangd"; now "except C5".
- 5.4 justified grouping by event with a variable initialized twice,
  which is not valid C. Now: one group per caller in valid C, and still
  well-formed groups under error recovery.
- A call inside a prototype (`int g(int a[sizeof f(1)]);`) makes a
  declaration-only function's outgoing non-empty; 5.4 and 5.6 say so,
  and clangd's answer is marked unprobed.
- 10.1's heading said "no compile database", but the second probe used a
  `compile_flags.txt`.
- Risk 2's list ran into its paragraph.

**7. Re-read of 3 to 4.4, 5.3 to 5.6 and 14 to 16 against the code.**
Found and fixed:

- `call_callee` was placed in cexpr.c, but the Q7 hook is in ccall.c.
  It now goes in cexpr_int.h, beside `ntag` and `first_child`; both
  files include it.
- `typedef_var.c:3:23` is a call (`make(1)`); "if `make` is called" was
  hedging a fact.
- The query goldens need no change: their only C reference lines are not
  callees (`query/csym.expected`).

Confirmed:

- the node tags `N_IDENT`, `N_CALL`, `N_MEMBER_EXPR`, `N_UNARY` and
  `N_PAREN`;
- `index_find_file` (index.h:192), `c_file`, `c_detail`,
  `cindex_decls_at`, `cindex_lower` and `cindex_scope_root`;
- only `decls.expected` and `typedef_var.expected` hold `REF func` lines.

**8. Read of 4.4 to 7.** Found and fixed:

- xcalls are not in `by_decl`, so "linear in one decl" did not hold for
  them. 5.1 now says how each query finds them.
- `text_params` cannot move to csymidx.c alone: its output struct `Sig`
  lives in features.c. `Sig` moves too, minus the LSP-only `doc` (6.3).

Re-checked: the matcher's `lea`/`lf` claims, by hand against the
segment rule.

**9. Read of 1 to 2 and 9.** Found and fixed:

- The goals table still said prepare finds "the C function" only. It
  now includes file-level variables and their initializers' callees.

**10. Read of 13 to 16.** Found and fixed: this log's round 1 said the
`typedef_var` golden stays unchanged, against 4.2 (it gains ` call`).

**11. Read of 10 to 12.** Found and fixed: the diff script's request
list (10.2) did not include the file-level variables that prepare now
answers. Nothing else.

Rounds 7 to 11 together cover the whole document after round 6's fixes.
The findings shrank from design bugs (round 5) to single phrases
(rounds 9 to 11). Each later fix is a sentence or a table cell, re-read
in place. No uninterrupted read of all 16 sections has yet come back
clean; the next one should be by a fresh reader.

**12. Fresh reader: one uninterrupted read of 1 to 16, every cited
line re-read at 809a6c8, plus one `--dump-symbols` probe (K&R and
nested-prototype parameter events).** Found and fixed:

- **Item resolution after an edit** (5.5). The rationale "positions only
  shift with edits" was false: the item is the client's pre-edit copy,
  so its position goes stale, and a fresh check renumbers decls added
  before it. The planned test "a follow-up after an edit above the
  function" would have failed. A name step (`scope == 0`, DEF first)
  now follows `data.c`; the test names it (9).
- **Test harness** (9). Scenarios cannot replay a response, so every
  follow-up item is hand-written (`basic.json` does so today, without
  `data`). The E4 and edit tests need `data.doc` and `data.name` written
  in; 9 says so.
- **xcalls of non-functions** (4.4). The builder copied every body
  callee, so `#define RUN() fp(1)` or a field callee (Q7) would have made
  xcalls that outgoing had to filter. They are now made for `CIK_FUNC`
  only, which matches the memory figure, `xcall func` and E1; invariant
  4, 4.3 and the 5.1 comment follow.
- **Verify-carry and xcalls** (4.4). "A distinct role value" would print
  as `DECL` or `?`: `cindex_role_name` masks to the role bits. xcalls are
  now marked by the new `call` field.
- **Effects at a distance** (4.2). "The bit depends only on the syntax
  around the name" ignored an edited `#define`, which changes call bits
  and xcalls at invocations outside the damage. Now stated as reported,
  as B3_DESIGN.md 12.10 accepts.
- **5.6 vs 4.1.** 5.6 said every `__builtin_*` call returns before any
  event; an unknown `__builtin___*_chk` callee becomes an implicit decl
  (cexpr.c:2039), as 4.1's table says.
- **Estimate arithmetic** (12). B4a's low total is 490 + 200 = 690, not
  700, so B4 is 1,620 to 3,110; round 3's check had added 1,220 and 400
  to 1,630. ROADMAP.md (uncommitted) repeats 1,630 and needs the same fix.

Confirmed: every line cited in section 3 and 13 (pp.c 160-167 and 961,
ppexpand.c 1538 included); K&R identifier lists give REF param events
and `__typeof__(n)` a REF before `x`'s DECL (6.3); the LSP rules on
`selectionRange` ⊆ `range`, `fromRanges` relative to the caller (incoming)
or the item (outgoing), inlay hint kind 2 and
`workspace.inlayHint.refreshSupport`, and `SymbolInformation`.

**13. Second uninterrupted read of 1 to 16 after round 12.** Nothing
material. Two touch-ups: 5.5 step 1 now keeps only decls that can be
items (5.3), so a stale position landing on a same-named local cannot
win over the name step; a broken line in 4.3 was reflowed. Round 12's
edits read consistently with 4.1, 4.5, 5.1, 9, 11 and 13 (risk 8 names
the name fallback).

**Still unverified:** the estimates. (The xcall placement for
`ID(CALL_LEAF(2))` was confirmed by the B4a dump: section 17.)

## 17. B4a as implemented (Round 218)

Implemented as written (all D and Q decisions as recommended), with these
differences and findings.

- **xcall placement confirmed.** `--dump-symbols` now prints xcalls
  (`FILE:L:C xcall func NAME -> #N`). For `ID(CALL_LEAF(2))`
  (tests/symidx/calls.c line 35) the xcall is at `ID`, col 10, the outermost
  invocation, as inferred in round 13. 4.4 stands.
- **Carry and a name adjacent to the damage.** 4.2 expected no difference
  in the `call` bit for names beside an edit. `carry/calls_adj` (`f;` to
  `f(9);`) shows the adjacent name's bit differs (2 differences between the
  carried and the fresh index): the name just before the inserted `(9)` is
  kept untouched by the carry (it lies outside the damage) but is a call
  only in the fresh run, because the call-ness of a name depends on the
  next token. The golden records the 2 differences; the carried index is replaced
  by the fresh one when the check publishes.
- **Weak-macro prepare.** prepare tries a strong macro, then C items, and
  keeps a weak-macro (macro with history only) fallback when no C item
  exists, as before B4a.
- **`cereal query` mode test.** `query_c` distinguishes callers from
  callees by `kind[5]` (`callers`/`callees` differ at index 5, not 4).
- **Changed golden.** `tests/query/calls.expected` last line: a C variable
  at `calls.c:7:5` now prints `no callers` instead of `no macro definition
  here` (the C path answers).
- **Macro items in an unopened header** resolve through `data.doc` and the
  macro index; a macro with no macro-level callers answers `[]` (macro
  hierarchy is macro to macro, as before).
- **Parked still:** a call in a file-level `_Static_assert` has no caller
  item (section 15).

## 18. B4b as implemented (Round 219)

Implemented as written (D1-D6 and Q1-Q7 as recommended), with these
differences and findings. The `bench/tools/clangd_diff.py` script is here too.

- **Underscores on both sides (corrects the first row of 6.5).** clangd
  20.1.8 suppresses `under(Count, _flag)` for `(int __count, int _flag)`:
  it strips the leading underscores of the argument as well as of the
  parameter name. 6.5 had recorded `flag:` for `under(count, _flag)`; the
  script found the difference on the golden's own case, and `arg_says_name`
  now strips both. `hints.expected` lost that one hint.
- **`text_params` moved with its struct.** `CSig` (label, offsets and the
  new source spans, `variadic`) and `csig_params` live in csymidx.c;
  features.c's `Sig` is `{CSig c; doc}`. The signatureHelp goldens
  (`csym_b3`, `ccalls`, `carry_cidx`) did not change.
- **Param-name check.** `--verify-symbols` compares the text rule with the
  index for every function declarator whose parameters it can read (K&R,
  spans with macro events, `;`, `{` or `\` in the span, and spans whose
  identifiers have no event are skipped). The count is printed as `N param
  misses` and symcov carries it (`--misses` lists them); it adds to the
  exit status of `--verify-symbols` but not to symcov's failure. Three false
  positives in gcc.dg (`typeof`, `__typeof__`, a `\u` universal character
  name) were fixed in the rule.
- **The view of a request is derived, not a table column.** `CP_VIEW`
  needs to know which view's refresh the client takes; a `view` field in the
  request table triggered `-Wmissing-field-initializers` on every other
  row, so `handle_request` derives it from the kind (`R_HINTS` is
  `VIEW_HINTS`, the semantic token kinds `VIEW_TOKENS`).
- **Call-hierarchy stress.** `tests/lsp_stress.py --calls` (and `--hints`)
  race prepareCallHierarchy, incomingCalls and outgoingCalls on hand-built
  `data` {name, doc} items against edits. The final line now also counts
  error responses.
- **`clangd_diff.py`.** Compares inlay hints and call hierarchy (B4c's
  workspace/symbol requests are not made) on `bench/clangd_diff/` (the
  probe cases: hints.c is the unit test's file, calls.c, symcalls.c, with a
  user and a system header) and three corpus files (lstrlib.c, lparser.c,
  libuv core.c; missing ones are skipped). It takes a few seconds.
  Result: 6 files, 294 differences, **0 unexpected**: C1 62, C3 29, C5 5,
  H1 146, H2 1, H3 1, H6 1, and two new ones below (C6 47, G1 2). H4 and H5
  show no difference on this corpus (clangd also takes the definition's
  names for `pdef`; the fake glibc header agrees).
  - **C1 widened:** clangd also lists non-call references in the outgoing
    direction (`int (*fp)(int, int) = leaf;` gives `fp` an outgoing `leaf`).
  - **H1 widened:** a callee spelled in a system header's macro
    (`CPU_COUNT(&set)` expands to `__sched_cpucount(...)`) gets hints from
    clangd and none from cereal.
  - **C6 (new):** a callee spelled in a system header's macro body
    (`assert` gives `__assert_fail`, `errno` gives `__errno_location`):
    cereal has the edge (xcall), clangd does not. Expected from 4.4; 4.4's
    option 2 text assumed clangd had them.
  - **G1 (new; closed in Round 221, section 19):** a function defined by a macro
    invocation (`DEFINE_FN(made)` in symcalls.c, whose body calls `leaf`)
    has no outgoing edge in cereal, and the xcall's caller is reported as
    file scope (`cereal query callers` prints it under `(file scope)`).
    Rule 1 of 5.1 says the function the root defines is the caller; the
    macro invocation's root evidently does not hold it as a DEF in the
    range the lookup scans. clangd reports `made` calling `leaf` sited at
    the invocation. Not fixed in B4b (a B4a change); fixed in Round 221.
  - **H3** here is the `int (*cb)(int depth)` field (`hints.c:45`).
- **Used `cereal/waitIdle`** to wait for cereal; clangd is polled with
  `workspace/symbol` until it knows the file's first symbol (no fixed
  sleeps).
- **Fix to the test script order.** `hints.json` writes its `.cereal`
  before `initialize`, like `csym_sys.json`; written after, `--flags` and
  the step both owned the file and the second removal failed.

Gates and sizes: run.sh 2074 passed (+6); gate.sh: san 2074 passed, 628
files, 0 findings; verify unchanged; callgrind 3.8832 G (3.8856 G before);
symcov `unindexed 0, param misses 0` over 4718 files, 1,146,276 events;
TSan: 54 session runs clean, `lsp_stress.py` 200 edits on zstd.c (plain, and
with `--calls --hints`) and `--calls --hints` on `tests/lsp/ccalls/main.c`:
0 reports, 0 error responses. Source +745/-169, hand-written tests ~170
lines (goldens 513), clangd_diff 318 plus the 130-line corpus. Estimate in
12: 360 - 700 src and 120 - 250 tests; the src is at the top.

## 19. B4c as implemented (Round 220)

Implemented as written (Q2 and Q6 as recommended: open units only, system
headers left out; E6: its own handler, no wait, no check started), with these
differences and findings.

- **Matcher: two rules added, one tightened (found by clangd_diff).** The
  segment starts of section 7 were lower-to-upper, after `_`, at a
  letter-digit change and at index 0. Against clangd the sets of matches
  disagreed in three ways, all fixed in `ws_match`:
  - `str` did not match `TString`: clangd treats the capital before a
    lower-case letter that follows a capital as a segment start (the `S`).
    Added.
  - `lf` matched `luaC_fix`, `LUA_INTEGER_FMT`, `lua_CFunction`: clangd
    skips no word, so a character that is neither right after the previous
    match nor at the *next* segment start after it does not match. Section 7
    said "at a segment start"; it is now "at the next segment start". The DP in
    `ws_match` resets its running minimum at each segment start.
  - `lea` matched `luaK_exp2anyreg` (`a` after the `2`, a letter-digit
    segment start); the next-segment rule removes it too. Whether clangd
    starts a segment at a letter-digit change was left open here and probed in
    Round 221 (below): it does not.
  `tests/lsp/wsym` pins `lc` (matches `luaC_fix`), `str` (`TString`) and `lf`
  (nothing).
- **One entry per decl per unit, not per decl.** A function defined in a.c
  and declared in a shared header, with b.c open too, is listed twice: at its
  definition (unit a.c) and at its prototype (unit b.c, which has no
  definition). The dedupe is by (path, offset, name) as written, so it
  collapses the copies of one place, not the two places. This is what 7 says
  and clangd does the same with a declaration it has no definition for; it is
  noted because the golden shows it.
- **`c_symbol_event` also rejects `CIX_SYSTEM` events** (7 says "not
  `body`/`system`"; the old document-symbol filter only had the body bit,
  since it saw user files only). Document symbols are unchanged.
- **Shape.** `lsp_workspace_symbols` (features.c) takes the referenced
  snapshots and indexes (`WsUnit`) and sorts all matches with `ws_cmp`
  (rank, macros last, length, name, path, offset, unit) before cutting at
  100, so the dedupe is a pass over neighbours. Declarations are found per
  decl through the `by_decl` CSR, matching the name first, so the event scan
  only runs for names that match. No allocation beyond the candidate list,
  a file map and the output.
- **Test harness.** The held-check case needs a barrier for "the snapshot
  exists": a `foldingRange` request (CP_NONE still waits for the first
  snapshot) because `waitIdle` waits for the check and the diagnostics of a
  held unit are not published. A closed document reschedules its unit, whose
  held check would block the builder, so the scenario has an `idle` after the
  close.
- **`clangd_diff.py` (workspace/symbol).** 13 queries per file. The
  main file's symbols must agree exactly. New and widened ids:
  - **W1** system symbols only in clangd (as designed).
  - **W2 (widened)** a user header's symbol only cereal lists. clangd's
    dynamic index gives it partly: forward declarations (`struct CallInfo;`),
    include guards, and for libuv's core.c no header symbol at all.
  - **W3 (widened)** kinds: macros 15 for clangd; also struct 5 (Class) and
    enumerator 10 (Enum) where cereal says 23 and 22.
  - **W4** differences where either answer hit the 100 limit.
  clangd's answers vary from run to run until its index settles: the script
  now waits for the file's diagnostics, then repeats each query until the
  answer stops changing. The earlier count of C3 (29) came from a clangd
  that had not settled; it is 8 now.
  Result: 6 files, ~1,700 differences (varies with clangd's system-header
  answers), **0 unexpected** over three runs.
- **`tests/lsp_stress.py --symbols`** sends a workspace/symbol request with a
  random short query in each round.

Gates and sizes: run.sh 2076 passed (+2); gate.sh: san 2076 passed, 628
files, 0 findings; verify unchanged (gcc.dg 3908 of 3910, c-c++-common 637 of
638, cpp 289 of 289); callgrind 3.8814 G (3.8832 G before, -0.05%); symcov
unchanged (`unindexed 0, param misses 0`, 4718 files, 1,146,276 events).
TSan (`~/cereal-tsan`, removed): 56 session runs clean (28 sessions, plain and
parallel+cells); `lsp_stress.py --symbols` 200 edits on zstd.c (239 edits sent),
and with `--calls --hints` on zstd.c and on `tests/lsp/ccalls/main.c`: 0 reports,
0 error responses. Source +276/-9 (features.c +227, server.c +41, lsp.h +8);
hand-written tests ~87 lines (53 of session JSON, 25 of C, 9 net in the stress
script) plus 2,773 lines of goldens (wsym.expected; 27 goldens gained the
capability line); clangd_diff +52/-4. Estimate in 12: 220 - 450 src and 80 - 160
tests: the src and the tests are below it.

G1 and C6 (the B4b open questions):

- **G1, root cause.** `csx_unit_begin` gives the root scope of an external
  declaration the range from `tok_begin` of its first token to `tok_end` of its
  last (csymidx.c:593). A token produced by a macro expansion is placed at its
  presentation point, the invocation's first character, and `tok_end` is
  `exp + 1`. So `DEFINE_FN(made)` has the root `[3:1, 3:2)`, one byte wide.
  The xcall of `leaf` at 3:1 is inside it (so it is attributed to a root),
  but `made`'s DEF is the argument token at 3:11, outside it: rule 1 of
  `caller_of` finds no DEF in the range, rules 2 and 3 find no caller event,
  and the site is "file scope". Outgoing from `made` finds no events in its
  body for the same reason.
  **Fixed (Round 221)** in the one place both lookups share: `widen_root`
  (csymidx.c, about 25 lines, first statement of `csx_unit_end`) extends the
  root's end to the largest `off + len` of the unit's events (from `unit_ev0`)
  that start inside the root in the same file, so the invocation's arguments
  are inside. Golden `tests/query/gmacro`: `made` and `other` have `leaf` as
  an outgoing callee and `leaf`'s callers list both. clangd_diff: the two G1
  lines are gone and the G1 class was removed from `classify_call`.

Gates (Round 221): run.sh 2079 passed (+3); gate.sh san 2079 passed, 628
files, 0 findings; verify unchanged; callgrind 3.8810 G (3.8814 G); symcov
unchanged; clangd_diff 7 files, 1958 differences, 0 unexpected; TSan 58
session runs and two 200-edit `--symbols --calls --hints` stress runs clean.

**Round 221: the digit rule probed.** `bench/tools/clangd_diff.py` machinery
against clangd 20.1.8 (`-nostdinc`, so the 100-result limit is not filled by
system headers): digits are lower case for clangd, so a letter-digit change is
not a segment start, and the section 7 rule is replaced by the probed one.
clangd matches the open file's symbols with its FuzzyMatcher alone and header
symbols with Dex (trigram tokens) plus the same matcher. `ws_match` is now a
port of both; its former dynamic program is gone. Over 95,758 pairs: open-file
symbols and macros agree exactly; header symbols differ in 3 pairs, all
queries with two underscores (`L__`, `l__` on `LUA_INTEGER_FMT`; `_ct` on
`SSL_CTX_new`), which depend on Dex's token details not reproduced. (Round 237 fixed
these: the query's trigrams run over its non-separator characters; see there.) Pinned by `tests/lsp/wsdigits` (a, ud, u8, 2b, ip4, s256, sha, 256,
x64, xs, ad, cd, aB, a1, SZ, bu16, i4; header-resident lf, lc, gf, gc, hdr_i,
hi4, hu8) and the `wsdigits.{c,h}` clangd_diff corpus (`EXTRA` queries).
Rank semantics: rank 3 + jumps now counts jumps over the new matcher (the
first match is free), so a few results change order against Round 220.
- **C6, decision.** Keep as an intended difference. A call spelled in a system
  header's macro body (`assert` to `__assert_fail`, `errno` to
  `__errno_location`) is a real call at the invocation; clangd's hierarchy
  omits them, and a filter that hid them would cost one `CIX_SYSTEM` test on
  the xcall's callee. Not done; say so if clangd's behaviour is preferred.
