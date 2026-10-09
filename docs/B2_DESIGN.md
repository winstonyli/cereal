# B2 design: the C symbol index

Status: designed 2026-10-09; phases 1 to 3 done (Rounds 187, 190 and
192, see the results sections at the end, which correct the text where
the implementation departs from it); phase 4 (rename) open. ROADMAP
Track B, B2. Line numbers refer to the WSL tree
`~/cereal-t`, which is the source of truth for code; the `src/c/` files
cited are identical in both trees.

## 1. Goals

B2 adds C-level navigation to `cereal lsp`. Macro lookup keeps
precedence. The index serves these requests:

| LSP request | Today | After B2 |
|---|---|---|
| `textDocument/definition` | macros, includes, macro params | plus C decls: the defining event, or every decl if none defines |
| `textDocument/declaration` | aliased to definition (`R_DEF`) | its own `R_DECL`: non-defining declarations (falls back to definition) |
| `textDocument/references` | macros | plus C decls; `includeDeclaration` honoured |
| `textDocument/documentHighlight` | not advertised | new: macros and C (Write for decl/def events, Read for uses) |
| `textDocument/hover` | macro expansion text | plus C hover: type line or record layout |
| `textDocument/prepareRename`, `rename` | macros only | plus C decls, under the rules in section 9 |

Covered entities: functions, objects (globals, locals, statics),
parameters (prototype, definition, K&R), typedefs, enumerators, struct
and union fields, labels (ordinary and `__label__`), and struct, union
and enum tags.

Not in B2:
- Document symbols, which B3 builds from this index.
- Cross-TU (project-wide) references, which are B5.
- Enum constants used in `#if`. The preprocessor sees an identifier
  there, not the enumerator, and evaluates it as 0.
- Completion.

## 2. What exists today (verified)

- **Two build phases.**
  - `builder_main` (server.c) publishes the macro snapshot first.
  - It then runs `check_run`: a fresh `TU` with its own interner and
    SrcMgr, with `fo.check = true` and `cancel = &u->cancel`.
  - `check_run` calls `frontend_run` (frontend.c), which runs
    `checker_new`, then `checker_unit` for each external declaration,
    then `checker_finish`, then `checker_free` (lines 171-244).
  - The checker is single-threaded. `cells_par` only changes where
    tokens come from.
  - Diagnostics are published under `S.m` only if `u->want == want` and
    the unit was not cancelled.
  - Units over `CEREAL_LSP_CHECK_MAX` (4 MiB) are skipped.
  - The check TU is freed at the end of the phase.
- **Separate location spaces.**
  - The check TU's `SrcLoc`s and `Ident` ids are not the snapshot's,
    because each phase has its own interner and SrcMgr.
  - `srcmgr_load` caches one `SrcFile` per normalized path. So the pair
    (normalized path, byte offset) is the only key shared by the two
    phases.
  - Both phases read the same overlay text.
- **Token provenance.**
  - Each token is a `PTok{t, exp, mloc}`. `t.loc` is the spelling
    location, `exp` is the presentation point (the outermost expansion
    site), and `mloc != 0` means the token came from a macro.
  - Flags: `TF_ORIGIN_BODY`, `TF_ORIGIN_ARG`, `TF_PASTED`, `TF_SYNTH`.
    A token can carry both ORIGIN bits: a body token of a macro whose
    expansion was itself an argument.
  - The code base uses `BODY && !ARG` for "spelled in a body" in three
    places: index.c:1841, check.c:77 and cstmt_warn.c:601.
- **Checker symbols.**
  - `CSym` kinds are `CS_OBJ`, `CS_FUNC`, `CS_TYPEDEF` and
    `CS_ENUMCONST`.
  - `gsyms` persist for the TU. `lsyms` are reset at the start of each
    `checker_unit` (check.c:673). A `SymRef` with `SYM_LOCAL` set points
    into `lsyms`.
  - Bindings go into one log with `top[ns][ident]`, where ns is
    `NS_ORD` or `NS_TAG`. A tag binding's ref is a TypeId.
- **Redeclarations.**
  - `cdecl_pushdecl` (cdecl.c:1506) returns the existing symbol when a
    redeclaration merges, so redeclarations share one CSym. Merging goes
    through duplicate_decls, `c->ext` (block-scope `extern`) and visref.
  - A conflicting redeclaration gets a new CSym.
  - Merged flags are ORed (cmerge.c:727), so `CSF_DEFINED` on the merged
    CSym does not say whether this particular declaration defines it.
- **pushdecl callers.** There are six:

  | Site | Declares |
  |---|---|
  | cdecl.c:2531 | typedefs |
  | cdecl.c:2555 | other declarators |
  | cdecl.c:2938 | prototype and definition params |
  | cdecl.c:3448 | function definitions |
  | cdecl.c:3627 | K&R implicit-int params |
  | crecord.c:1005 | enumerators |

- **Fields.**
  - Members are collected as `FieldIn` in `c->fields`
    (`cdecl_member_visit`, crecord.c:1275).
  - `struct_finish` (crecord.c:576) compacts them (`f[m++]`) and calls
    `type_complete_record` at line 713. That fills `TT->fields`.
  - Field indices are stable for the TU. `find_field` (cexpr.c:579)
    returns a pointer into `TT->fields`.
- **Labels.** `new_label` (cstmt.c:231), `define_label` (253),
  `use_label` (288) and `__label__` (`local_labels`, 1454). Label slots
  are reused after a nested function pops.
- **Hover text.** `dump_decl` (cdecl.c:1704) prints the `--dump-types`
  line. `type_dump_record` prints a record layout.
- **Macro index.**
  - `index_resolve` (index.c:1563) tries, in order: includes, then
    definitions and params, then covering refs, then a plain-identifier
    fallback (lines ~1664-1685).
  - The fallback returns `TGT_MACRO` for any name with macro history,
    even when the token under the cursor is an ordinary C identifier
    such as a variable that a macro once shadowed.
  - `Index.inactive` lists inactive `#if` ranges for every file of the
    TU (`lsp_publish_inactive` filters them per file).
- **Tests.**
  - LSP: `tests/lsp/{basic,diag,proj}.json` with `.expected` goldens,
    driven by `tests/lsp_session.py`. `run.sh` runs each scenario
    serially and with `-fparallel=on -fparallel-chunk=1
    -fparallel-threads=3`.
  - `tests/query/*.cmd` drive `cereal query def|refs|hover`.

## 3. Architecture

```
check_run -> frontend_run(fo.symidx = true)
               checker_new(co.symidx)      Checker.sx = csymidx_new()
               checker_unit*               hooks record events into sx
                 unit end                  hover for locals, labels -> strings
               checker_finish
               csymidx_finish(sx) -> CIndex (immutable, malloc'd)
               checker_free                TU-side tables gone, CIndex kept
builder_main  (under S.m, same publish as diagnostics)
               validate file hashes -> snap->cidx = idx
requests      cidx captured with the snapshot ref; C lookup after macro lookup
```

- **New module.** `src/c/csymidx.[ch]` holds the builder (`SymIdxB`,
  private to src/c) and the frozen `CIndex` (public, with no checker
  types).
  - The checker's tables stay private: all code that reads `CSym`, `TT`
    or `c->fields` lives in src/c.
  - The LSP sees only `CIndex`.
- **Off by default.** Every hook is `if (c->sx) csx_...(...)`. `cereal
  check` and the dogfood runs keep `sx == NULL`, so they pay one branch
  per hook.
- **Plumbing.**
  - `CheckOptions.symidx` (bool).
  - `FrontendOpts.symidx` (bool) plus `FrontendOpts.cidx_out`
    (`CIndex **`).
  - `frontend_run` calls `csymidx_finish` between `checker_finish` and
    `checker_free` (frontend.c:243/244). It also calls it on a cancelled
    run, but the server discards that result.

## 4. Data model

### 4.1 Decl ids

A decl id is a dense `u32` in first-seen order. It is stable within one
CIndex only. During the build, a key is mapped to an id as follows:

| Entity | Builder key | Notes |
|---|---|---|
| global ord symbol | gsym index | redeclarations that merged share it; conflicting types get a new CSym and so a new id (correct: gcc treats it as an error and a new decl) |
| local ord symbol | lsym index through a per-unit map | the map is cleared where `lsyms.len = 0` |
| tag | canonical record/enum TypeId (hash map) | `struct S;` in an inner scope gives a new TypeId and so a new id |
| field | `TT->fields` index | anonymous members are indexed but not named |
| label | id assigned in `new_label` and stored in a side array by label slot | slot reuse after nested functions cannot alias |

Map value 0 means the key has not been seen yet; ids start at 1. Two
cases are special:
- A `CSF_ERROR` placeholder from `undeclared_` (cexpr.c ~1766) is never
  given an id.
- Decls created with location 0 or in system headers get their id lazily
  on first use. Those with location 0 are the predeclared typedefs
  `__builtin_va_list`, `__int128_t`, `__uint128_t` and `nullptr_t`.

### 4.2 CIndex (frozen)

```c
typedef struct CIdxFile {            /* files that have events */
    const char *path;                /* normalized, as srcmgr has it */
    uint32_t size; uint64_t hash;    /* of the text the check read */
    bool stale;                      /* set at publish (section 7) */
} CIdxFile;

typedef struct CIdxEvent {           /* 12 bytes, sorted by (file, off) */
    uint32_t off;                    /* byte offset in the file */
    uint32_t decl;
    uint16_t file;
    uint8_t  len;                    /* 0: longer than 255, re-lex */
    uint8_t  flags;                  /* role:2 DECL/DEF/REF/WRITE?,
                                        MACRO_BODY, AT_EXPANSION, ARG,
                                        SYSTEM */
} CIdxEvent;

typedef struct CIdxDecl {            /* 16 bytes */
    uint32_t name, hover;            /* string pool offsets */
    uint32_t scope;                  /* scope id: 0 file, else block */
    uint8_t kind;                    /* func obj param typedef enumconst
                                        field tag label */
    uint8_t linkage;                 /* none internal external */
    uint16_t flags;                  /* BUILTIN IMPLICIT SYSTEM TENTATIVE */
} CIdxDecl;

typedef struct CIndex {
    CIdxFile *files; uint32_t nfiles;
    CIdxEvent *ev;   uint32_t nev;
    uint32_t *by_decl, *by_decl_start;  /* CSR: event indices per decl */
    CIdxDecl *decls; uint32_t ndecls;
    CIdxScope *scopes; uint32_t nscopes;  /* parent, file, begin, end */
    char *strings;                         /* deduplicated pool */
} CIndex;
```

- **Event roles.**
  - **DEF:** a function definition, an object with an initializer or a
    definition at block scope, a typedef, an enumerator, a field, a tag
    with a body, a label definition, or a parameter of a function
    definition.
  - **Tentative definitions.** One gets the DEF role only if the decl
    has no other DEF event. This is decided in `csymidx_finish`.
  - **DECL:** any other declaration, including prototypes, `extern`
    declarations, `struct S;` and forward references that create a tag.
  - **REF:** a use.
  - The role is decided at the hook from the declaration being
    processed (its local flags, set at cdecl.c:2453, 2519, 3424, 1223
    and 3624). The merged CSym's flags cannot be used, because merging
    ORs them (section 2).
- **Lookup.** A query position (file, off) is answered by binary search
  over `ev`. Several events may share one position (section 5.3).

### 4.3 Memory

Measured identifier counts after preprocessing (the scratch script
`count_ids.py` over `cereal -E`), with the parse-dump event nodes that
the hooks would see:

| Unit | Ident tokens: main / user hdrs / system | Event nodes (IDENT, NAME, TYPEDEF_NAME, MEMBER, TAG, ...) |
|---|---|---|
| src/main.c | 2,328 / 4,772 / 2,313 | 8,726 |
| src/c/cexpr.c | 19,549 / 5,869 / 3,995 | 27,945 |
| zstd.c (2.2 MB amalgamation) | 73,116 / 0 / 10,861 | 80,231 |

- **Per event:** 12 bytes, plus 4 bytes in the CSR array, so about 16
  bytes.
- **Per decl:** 16 bytes, plus its name and hover strings. Hover
  strings average about 40-80 bytes. Many decls share a name, and the
  pool deduplicates names and identical hover lines. Allow about 60
  bytes per decl in total.
- **Retained, by unit:**

  | Unit | Retained CIndex |
  |---|---|
  | zstd.c | about 80k events and 10-20k decls, so 1.3 MB + 0.6-1.2 MB ≈ **1.9-2.5 MB** |
  | cexpr.c | **≈ 0.6-0.9 MB** |
  | main.c | **≈ 0.2-0.4 MB** |

- **Worst case.** An identifier-dense 4 MiB file (one identifier per 4
  bytes, about 1M events) is **≤ 20 MB**.
- **Builder overhead.** The builder holds the same data unsorted, plus
  hash maps, for the length of the phase. That is about 2× the retained
  size, small next to the TU (main.c's check TU is tens of MB).
- **Peak memory.** The 830 MB peak (ROADMAP) is the snapshot phase. B2
  adds at most about 40 MB transient and 20 MB retained per unit.
- **Retention across units.** Only the newest checked snapshot of each
  open unit keeps its CIndex, and older snapshots free theirs when their
  last request ends. This bounds the total.
- **Verify after phase 1.** Record the measured CIndex bytes for the
  three units above in LSP.md, replacing these estimates. Done (Round
  187): main.c 0.20 MB, cexpr.c 0.52 MB, zstd.c 1.37 MB (65,898 events:
  fewer than the 80k nodes because system-presented events are dropped).
  16 B per event as estimated; 25 B per decl without hover, about 83 B
  with hover projected from `--dump-types` line lengths (1.4x the 60 B
  allowance, within the 2x tolerance). Measured with hover (Round 192):
  about 30-39 B per decl added, so 55-64 B in all, because equal texts
  share one string (zstd.c 1.37 to 1.81 MB, cexpr.c 0.53 to 0.68 MB,
  main.c 0.21 to 0.34 MB). The dense worst case (a 4 MiB struct of
  330,869 `int fN;` members, one decl and one distinct text each) is
  29.3 MB, up from 14.4 MB; see "Phase 3 results".

## 5. Hook sites

Each hook names file:function and what is in hand there. "tok" is the
name token. Its spelling location is `in[tok].t.loc` and its
presentation point is `in[tok].exp`.

### 5.1 Declarations

| Hook | What is available | Records |
|---|---|---|
| cdecl.c:`cdecl_pushdecl` (1506) gains a `uint32_t name_tok` parameter, passed by all six callers | the returned SymRef (merged or new), the declaration's own flags in `xin` | DECL or DEF event for the returned symbol |
| cdecl.c:`declared_visit` 2531/2555 | `g.name_node` (set at 656), so `n[name_node].tok` | passes tok |
| cdecl.c:`param_visit` 2938 | `g.name_node` | passes tok; unnamed params pass 0 and record nothing |
| cdecl.c:`funcdef_declared` 3448 | `g.name_node` | passes tok, DEF |
| cdecl.c:`body_visit` K&R loop (~3600-3627) | each `N_KR_IDENT` node's tok; params already bound are re-bound at 3546 with the same ref | DEF event at the KR_IDENT tok for the matched param; the later declaration in the declaration list goes through 2938/2555 as a DECL of the same ref |
| crecord.c:`cdecl_enumerator_visit` 1005 | the enumerator node's tok | DEF |
| crecord.c:`cdecl_member_visit` 1275 | member node tok; add `uint32_t tok` to `FieldIn` (type.h) so it survives the compaction in `struct_finish` | nothing yet |
| crecord.c:`struct_finish` after `type_complete_record` (713) | final `Record.fields + k` for each kept `f[k]` | DEF event per named field (tok from `FieldIn.tok`) |
| crecord.c:`cdecl_open_visit` 140 | the tag token and the TypeId from `tag_here`/`new_tag` | DEF event for the tag |
| crecord.c:`xref_visit` 274, at the end next to `c->ty[i] = t` (337) | `kind` (TSK_TAGFIRSTREF if created), the tag token | DECL if it created the tag, else REF |
| cdecl.c:`cdecl_shadow_tag` 1899 (cbind at 1968) | ltok, the new inner TypeId; it runs after `xref_visit` resolved the same token to the outer tag | replaces the event at ltok with a DECL of the new tag |
| cstmt.c:`new_label` 231 / `define_label` 253 / `local_labels` 1454 | label slot, the token | assign id at `new_label`; DEF at `define_label`; DECL at `__label__` |
| check.c:`scope_open`/`scope_close` (580/593) and `cscope_push`/`pop` (492/500) | the scope's opening and closing tokens | the scope tree, for the rename conflict check |

### 5.2 Uses

| Hook | What is available | Records |
|---|---|---|
| cexpr.c:`e_ident` 1888 | `ref = lookup_ord(c, id)` at the top | REF when the ref is not `SYM_NONE`. Skip when the parent is an ATTR_ITEM whose attribute does not take expressions: factor the existing `ex[]` list into `attr_takes_expr(name)` and add `cleanup`. Then `format(printf, 1, 2)` records nothing, while `cleanup(fn)` and `alloc_size(n)` record. Skip `__func__` and `__builtin_*` (no symbol). |
| cexpr.c:`implicit_decl` 1652 | the CSym it reuses from `c->ext` or creates with `CSF_IMPLICIT` | REF (decl flag IMPLICIT if the symbol has no other decl) |
| cspec.c:`specs_visit` case `N_TYPEDEF_NAME` 604 | `ref = lookup_ord`; `NF_ERROR` set on unknown type names | REF unless NF_ERROR |
| ccall.c:`e_member` 2228 (find_field 2269) | the Field pointer, which gives the index | REF at the member token |
| cexpr.c:`e_offsetof` 5210 (find_field 5250) | the Field for each member designator | REF per member token |
| cinit.c:`set_init_label` 2128 (`lookup_path` 2142) | the field path; anonymous members appear as intermediate steps | REF for the last named Field, at the designator token |
| cstmt.c:`use_label` 288, reached from `N_GOTO` (cstmt_warn.c:294) and `N_ADDR_LABEL` (cstmt.c:1007; cexpr.c:`e_addr_label` 5670) | label slot | REF |
| check.c:`checker_unit` before `lsyms.len = 0` (673) | all lsyms of the unit, labels | format hover strings for local decls and labels (they disappear at the reset) |

Globals, fields and tags get their hover text in `csymidx_finish`, from
the final CSym and TT. Composite types are complete by then.

### 5.3 Event location and macro classification

For a token `p = in[tok]`:

1. **Presentation point outside the check set.** If `p.exp` is in a
   system file, record nothing. This keeps system-header internals out.
   A system decl still gets a lazy entry when a user file uses it, with
   one DECL event at `CSym.loc` flagged SYSTEM.
2. **Not from a macro** (`mloc == 0`). Use `t.loc`.
3. **From a macro, spelled in real source** (not pasted or synthesized).
   Use `t.loc`, the spelling. Then classify it in `csymidx_finish`:
   - If the spelling lies inside a `#define` body range of the check
     TU, flag it MACRO_BODY.
   - Otherwise it was written in source as a macro argument. Flag it
     ARG.

   Deciding by range rather than by the ORIGIN bits removes the
   ambiguity when both bits are set. The check TU's `pp` keeps the
   define ranges (`macro_ctx = &tu->pp` is already passed to the
   checker).
4. **Pasted or synthesized** (`TF_PASTED` or `TF_SYNTH`). There is no
   spelling, so use `p.exp` and flag it AT_EXPANSION.

Several events may share one location:
- A body token used by many expansions, such as a statement-expression
  macro that declares `_x` each time.
- A token that names several entities.

Exact duplicates (same location, decl and role) are dropped at finish.
When a location still maps to several decls:
- Navigation lists them all.
- Hover lists the distinct strings, up to 5, then "and N more".
- Rename refuses (section 9).

## 6. Query merge

1. Resolve as today: `t = index_resolve(...)`. Add
   `IdxTarget.weak = true` in the plain-identifier fallback
   (index.c:1664-1685).
2. If `t.kind != TGT_NONE` and `!t.weak`, the macro answer stands, as
   today.
3. Otherwise, try C:
   - Map the request position to (normalized `r->file->path`,
     `loc - r->file->base`).
   - Find the CIndex file entry by path. Skip it if it is stale.
   - Binary search for events covering the offset.
4. If C found decls, answer from them. Otherwise answer from `t`, which
   may be weak or NONE.
5. **Mapping results back.** For each event location, `index_find_file`
   on the snapshot (by path) gives the SrcFile, and `base + off` gives
   the snapshot SrcLoc. Then `json_location` as today.
   - When a file is not in the snapshot (only possible when the two
     phases saw different include sets), emit the URI from the path
     directly.
   - Positions come from the snapshot's line table. Hash validation
     guarantees the text is the same.

**Request behaviour.**
- **definition:** DEF events of the decl. If there are none, all DECL
  events, as clangd does for an extern with no definition in the TU.
- **declaration:** DECL events. If there are none, DEF.
- **references:** all events. With `includeDeclaration: false`, drop
  DECL and DEF. MACRO_BODY events are reported once at their spelling.
- **documentHighlight:** events of the decl in the requested file only;
  kind 3 (Write) for DECL and DEF, kind 2 (Read) for REF. For macros,
  reuse the reference walk restricted to the file.
- **hover:** the C hover is shown when step 3 answers. If the macro
  answer was weak, append a line "(also a macro name)".

## 7. Threading, lifetime and staleness

- **Builder thread.** The builder alone builds. `check_run` returns the
  check TU and the CIndex.
- **Publishing.** The CIndex is attached in the same critical section
  that publishes diagnostics (server.c `builder_main`), under `S.m`, and
  only if `u->want == want` and the run was not cancelled.
- **Hash validation.** Before attaching, compute the hash of each
  CIndex file against the snapshot's `SrcFile` text (by path).
  - A mismatch sets `stale`. This is possible when a file changed on
    disk between the two phases and was not open in an overlay.
  - The overlay is shared, so open buffers always match.
- **Snapshot fields.**
  - `Snapshot.cidx` is set once and is NULL until the check finishes.
    It is written only under `S.m`, and requests read it under `S.m`
    when they take their `snapshot_ref`.
  - `Snapshot.check_state` is one of NONE, PENDING or DONE.
  - `snapshot_release` frees `cidx` with the last ref.
- **No index.** A unit over 4 MiB, a cancelled check, or a unit opened
  as a standalone header gets no CIndex. C queries then answer as today
  (the macro result or null).
- **Edits.**
  - Every edit bumps `want` and builds a new snapshot. The new snapshot
    has `cidx == NULL` until its check publishes.
  - Default behaviour: a C query against a snapshot whose check is
    PENDING waits on the unit's condition variable up to a deadline
    (see decision D1). If the check does not finish in time, it answers
    from macros only.
  - The previous snapshot's CIndex is not reused, because offsets after
    the edit point are wrong.
- **Rename** requires `snap->gen == u->want` (no newer edit queued) and
  `cidx` present. Otherwise it is refused with "index is out of date,
  retry".
- **Cost to other units.** The check already runs on the single builder
  thread. B2 adds hook and finish time (target ≤ 10% of the phase,
  measured in phase 1). The ROADMAP risk that one unit's check delays
  another unit's snapshot is unchanged and still open.

## 8. Partial and erroneous code

| Situation | Behaviour |
|---|---|
| Parse error inside a declaration | the N_ERROR subtree is skipped; tokens in it get no events; the rest of the unit is indexed |
| Units with parse errors (`checker_unit(c, u, had_errors)`) | hooks still run; partial data is fine |
| Undeclared identifier | no event (the CSF_ERROR placeholder never gets an id) |
| Unknown type name (`NF_ERROR`) | no event |
| Implicit function declaration | REF to a decl flagged IMPLICIT; a later real declaration merges into the same CSym (`c->ext`) and so the same id |
| Conflicting redeclaration | new CSym, so a new id; navigation shows both decls; rename refuses because two decls share the name in one scope |
| Missing include (`fatal_missing_include`) | the check stops early, and the index covers what was checked (D2) |
| Cancelled check | no index |
| Inactive `#if` branch | not checked, so no events; rename uses it as a blocker (section 9) |
| Enum constant in `#if` | no event (the preprocessor sees an identifier); by-name fallback parked |

## 9. Rename (phase 4)

`prepareRename` and `rename` on a C decl are refused, with the reason
given, if:

1. Any event of the decl is MACRO_BODY, AT_EXPANSION, pasted or
   synthesized. An edit there would change the macro for every user.
   Events flagged ARG are allowed: the argument is spelled in the user's
   source.
2. The decl is SYSTEM, BUILTIN, IMPLICIT-only, or has an event in a
   system header.
3. The old spelling occurs as an identifier token in any inactive range
   of the snapshot (`snap->ix.inactive`, all files of the TU). Such a
   use cannot be classified.
4. The new name is not an identifier, is a keyword, or is a macro name
   anywhere in the TU (`mt_hist`).
5. **Scope conflict.**
   - The new name is already declared, in the same namespace, in the
     decl's scope or any scope nested in it where an event of the decl
     occurs. Renaming would make such an event bind to the other decl
     (capture).
   - Or renaming would shadow-capture a use of an outer entity with the
     new name inside the decl's scope.
   - Checked with the recorded scope tree and per-scope name lists.
   - Fields use the record as their scope.
   - Labels use the function.
   - Tags use `NS_TAG`.
6. Scope of the edit (decision D3). Default: the decl has no linkage or
   internal linkage, or every event is in the TU's main file. Otherwise
   it is refused with "symbol is visible outside this file; rename needs
   the project index (B5)".
7. More than one decl at the position (section 5.3).

The edit lists every event's range, deduplicated, in the existing
`"changes"` form.

## 10. Edge cases (how the design treats each)

| Case | Treatment |
|---|---|
| Shadowing | each binding has its own CSym (lsym or gsym), so its own id; `lookup_ord` resolves at the use, so uses follow C scoping exactly |
| Tags vs ordinary namespace | tags are keyed by TypeId, ordinary names by SymRef: `struct S S;` gives two decls on adjacent tokens |
| Same member name in two structs | keyed by global field index, so distinct |
| Forward declarations and redeclarations | merged CSym or canonical TypeId, so one id; a `struct S;` in an inner scope creates a new tag (shadow) |
| K&R parameters | identifier-list token gets the DEF; the declaration-list declarator a DECL of the same ref; implicit-int params via 3627 |
| Statement expressions | ordinary block scopes inside an expression; when written in a macro body, see section 5.3 (deduplicated body events, rename refused) |
| typeof | `typeof(x)` is checked like an expression, so `x` gets a REF; `typeof(T)` goes through N_TYPEDEF_NAME |
| Designated initialisers | `.a.b = ` records REF for each designator token via `set_init_label`/`lookup_path`; `[i]` designators are expressions |
| Compound literals | type name via specs (N_TYPEDEF_NAME, tags); the initializer list like any initializer |
| Enum constants in `#if` | not indexed (section 8) |
| Builtins | `__builtin_*` calls have no symbol (no event); predeclared typedefs get a lazy decl with no location: definition returns nothing, hover shows the type |
| Implicit function declarations | section 8 |
| static/inline functions in headers included by many units | each unit's CIndex has its own decl; within one TU events in the header are indexed normally; cross-unit references wait for B5; rename of a static function defined in a header is refused under D3 default (events outside main file) |
| Same header included twice under different macros | events at the same header offset may point to different decls, so the location is ambiguous (section 5.3) |
| Anonymous struct/union members | the intermediate anonymous field has no name, so no event; designators and `.x` through it reference the inner named field |
| Attribute arguments | handled by `attr_takes_expr` (section 5.2) |
| Bit-field widths, array sizes, `_Static_assert`, `alignas` | ordinary expressions, so indexed |
| `_Generic` associations | type names via specs; unselected association expressions are visited and indexed (settled by `tests/symidx/stmts.c`) |

## 11. Test plan

1. **`cereal check --dump-symbols FILE`** (phase 1). The output prints
   one line per event: `file:line:col ROLE kind name -> decl#` plus the
   decl table.
   - Goldens go in a new `tests/symidx/`, one small `.c` per edge-case
     group in section 10.
   - Each golden runs plain and with `--cells`, and both outputs must be
     equal.
2. **`cereal check --verify-symbols`** over `bench/corpus.py` and
   `src/*.c` (phase 1):
   - Every identifier token, after the section 5.3 classification and
     excluding keywords, attribute names and inactive code, has an event
     or a recorded reason (undeclared, attribute, builtin).
   - Every decl has at least one DECL or DEF event, except system,
     builtin and implicit decls.
   - Events are sorted and the CSR is consistent.
   - Hash checks pass.
   - Run it in the dogfood job.
3. **`cereal query def|decl|refs|hover|rename FILE:L:C`** gains C
   support. `tests/query/csym.cmd` covers the merge rules: a strong
   macro wins, a weak macro loses to C, NONE goes to C, and an ambiguous
   body location lists all.
4. **LSP scenario `tests/lsp/csym.json`** (run by `run.sh` in both
   modes). Each phase adds its own steps:
   - Phase 1: definition and declaration on a prototype in a header,
     then the definition in main.
   - Phase 2: references with `includeDeclaration` false; documentHighlight
     kinds.
   - Phase 3: hover on a field and on a typedef.
   - Phase 4: rename OK, then refused for each of: a use in a macro
     body, an identifier in an inactive branch, a capture, and extern
     linkage.
   - An edit then a request (waits for the check).
   - A missing include.
   - A harness `env` step to set `CEREAL_LSP_CHECK_MAX` small and check
     that a C query is null while macro queries still work.
5. **Sanitizers.** TSan run of the LSP scenarios (the publish and
   capture of `cidx`). ASan run of `--dump-symbols` over the corpus.
6. **Timing.** `cereal check` on zstd.c and cexpr.c, with the index on
   and off. Report best of N, the machine load and the Defender state.
   Record the measured CIndex bytes in LSP.md.

## 12. Phases

Each phase ships on its own, with its tests passing.

| Phase | Content | Est. lines |
|---|---|---|
| 1 Declarations and definition (**DONE**, Round 187; ~1,320 lines: csymidx.c/h 950 including dump and verify, 370 in existing files) | csymidx core (builder, maps, finish, sort, CSR, strings); all hooks of sections 5.1-5.2; `--dump-symbols`, `--verify-symbols`; `tests/symidx`; server plumbing (`cidx`, publish, hash check, D1 wait); `IdxTarget.weak` and merge; definition and a separate `R_DECL` | ~500 |
| 2 References and highlight (**DONE**, Round 190; ~290 lines written, net +190, against ~250 re-estimated) | references with includeDeclaration; documentHighlight provider and capability (macros too); query `refs` | ~80 |
| 3 Hover (**DONE**, Round 192; ~350 lines added, 66 removed, against ~300 re-estimated) | `dump_decl` refactored to write a StrBuf; unit-end and finish formatting; record layout and labels; hover merge | ~120 |
| 4 Rename | blockers 1-7, inactive scan, scope tree and conflict check, C path in prepareRename/rename | ~200 |
| Total | | **~900** |

The ROADMAP estimate was 400-600 lines and 6-8 hook sites. This design
needs about 20 hook points across 8 files, because fields, labels, tags,
K&R, designators and offsetof each have their own path. It also needs
server plumbing and a merge layer that the estimate did not include.
ROADMAP should be updated when phase 1 starts.

## 13. Risks

1. **Hook coverage gaps.** A path that binds or resolves names without
   passing a hook silently loses references, and rename would then
   produce wrong code. `--verify-symbols` over the corpus is the guard,
   and rename refuses any decl with an unclassified token nearby.
2. **Two location spaces.** The CIndex uses (path, offset), validated by
   a hash per file. A mismatch makes the file stale and never produces a
   wrong location.
3. **Macro provenance.** Body vs argument is decided by define ranges
   rather than by ORIGIN bits. A wrong classification would only make
   rename refuse or allow too much; navigation is unaffected.
4. **Builder-thread latency.** The check runs behind the snapshot on the
   one builder thread. A long check (1.7 s on a 4 MB table) delays other
   units and makes C queries wait. This is not new with B2, but B2 makes
   users notice it.
5. **Line estimate.** ~900 lines against 400-600.
6. **Rename safety across TUs.** Mitigated by the D3 default.

## 14. Decisions for the user

**D1. A C query while the check for the newest edit is still running:**
1. Answer from macros only until the check publishes. Simple, but
   right after typing, go-to-definition on a C name returns nothing.
2. **(Recommended)** Wait up to ~1.5 s for the PENDING check, then fall
   back to option 1. Needs `cond_timedwait` in thread.h (~40 lines).
3. Use the previous snapshot's index for files whose hash is unchanged.
   Answers instantly for other files, but not for the file being
   edited.

**D2. Missing include in the LSP check:**
1. Keep `fatal_missing_include`: the index stops at the missing
   include. Cheap, matches today's diagnostics.
2. **(Recommended)** Make the LSP check non-fatal, but drop compiler
   diagnostics after the first missing include. The index then covers
   the whole file, which matters most while a project is half
   configured, at the cost of a full check.

**D3. Rename scope for C names:**
1. **(Recommended)** Conservative: only no-linkage or internal-linkage
   decls, or decls whose events are all in the main file, until B5
   provides the project index.
2. TU-wide like the macro rename: edits headers, and may break other
   units that include them.

**D4. Names spelled in macro arguments:** allow rename
**(recommended)**, or refuse every token that went through a macro.

## 15. Review log

Six rounds (five adversarial, one verification-only) were run against
the draft, each re-checking
claims against the code (grep or read in `~/cereal-t`).

1. **Round 1: stale and wrong claims.**
   - The draft decided DEF from the merged CSym's `CSF_DEFINED`. But
     cmerge.c:727 ORs `CSF_DEFINED` into the merged symbol, so a
     prototype after a definition would be marked DEF. Fixed: the role
     comes from the declaration's own flags at the hook.
   - Confirmed the six `cdecl_pushdecl` callers and `g.name_node`.
   - Confirmed that the checker is single-threaded (frontend.c has no
     thread use; the checker runs at 171-244).
2. **Round 2: tags.**
   - The draft placed the tag hook in `cdecl_tag_visit`, which only logs
     an iloc event. Moved it to `xref_visit` (`c->ty[i] = t` at 337).
   - `struct S;` in an inner scope runs `xref_visit` (resolving to the
     outer tag) before `cdecl_shadow_tag` creates the inner tag (called
     from crecord.c:1361 and cdecl.c:2063). Added the replace-event rule.
3. **Round 3: fields and labels.**
   - `struct_finish` compacts `FieldIn` before `type_complete_record`,
     so a hook that remembered positions in `c->fields` would map to the
     wrong field. Fixed with `FieldIn.tok` and a hook after line 713.
   - Label slots are reused after a nested function pops, so ids are
     assigned at `new_label`.
   - The `undeclared_` CSF_ERROR placeholder must never get an id.
4. **Round 4: macros and locations.**
   - The ORIGIN bits can both be set, so the draft's `BODY && !ARG`
     rule would hide argument tokens inside nested expansions. Replaced
     by classifying against define ranges.
   - Pasted and synthesized tokens have no spelling; they now use `exp`.
   - Attribute arguments (`format(printf, ...)`) would have been
     recorded as uses of `printf`; added `attr_takes_expr`.
   - The plain-identifier fallback in `index_resolve` would hide every C
     name that once had macro history; added `weak`.
5. **Round 5: lifetime, staleness and the full edge-case list.**
   - Confirmed that `Index.inactive` covers all files of the TU (so the
     rename scan covers headers).
   - Confirmed that the two phases have separate interners, so
     (path, offset) plus a hash is needed.
   - Added: rename gated on `snap->gen == u->want`, a cancelled check
     drops the index, and standalone headers get no index.
   - Walked every edge case in section 10; added the rows for a header
     included twice under different macros and for anonymous members.
   - A sixth, verification-only pass corrected the `checker_unit`
     parameter (`had_errors`, not `quiet`) and marked the `_Generic`
     claim as unverified. After that, a final pass found nothing further
     of substance. Still unverified
     until phase 1 runs: the memory figures (estimates from token counts
     and struct sizes) and the ≤ 10% time overhead target.

## Decisions (user, 2026-10-09)
D1: a C query during a running check waits up to about 1.5 s (`cond_timedwait`), then answers from macros only.
D2: the LSP check is non-fatal on a missing include; compiler diagnostics after the first missing include are dropped so the index covers the whole file.
D3: rename only names with no or internal linkage, or used only in the main file, until B5.
D4: renaming a name written in a macro argument is allowed.

## Phase 1 results and corrections (Round 187)

Phase 1 is done (HISTORY.md Round 187; LSP.md "C symbol index"). Where
the implementation departs from or settles the text above:

1. **D1 wait condition.** The wait covers not only a PENDING check but
   also a newer edit whose snapshot is queued or still building
   (`snap->gen < want`); otherwise a request right after an edit would
   answer at once from the previous snapshot.
2. **Files absent from the snapshot** (read by the check but not by the
   macro phase) are skipped rather than emitted from their path: without
   the snapshot's line table there is no offset-to-position conversion.
3. **`--verify-symbols` excuses** a resolved identifier with no event
   when a warning or error was diagnosed on the same line (all misses
   over `tests/check` were in erroneous code: dropped fields, duplicate
   parameters, bad designators, labels outside functions), plus
   `__builtin_*` and `__func__`-like names, error nodes, attribute
   subtrees, quiet units and system presentation. Clean over
   `tests/check`, `tests/parse`, `tests/symidx`, cereal's sources, all of
   `bench/corpus.py` and 4716 gcc.dg and c-c++-common tests (Round 188);
   run in `tests/run.sh` section 14. `bench/tools/symcov.sh` reruns the
   Round 188 coverage (one summary line, unindexed names, exit 1 if any;
   `--gnu11` for the second pass).
4. **Kinds** print as `struct`, `union`, `enum` (not one `tag` kind).
5. **Parameters:** a prototype's parameters are DECL; a body (or K&R
   declaration list) upgrades them to DEF (`csx_param_def`).
6. **Line count:** about 1,320 lines against the ~500 estimated for
   phase 1; the four-phase total of ~900 is no longer realistic.
7. **Re-estimate of phases 2 to 4** (made before phase 2 started, from
   phase 1's 2.6x overrun: 1,320 lines against 500). Phase 1 overran
   because the per-path hooks, the `--verify-symbols` tool and the server
   plumbing were each larger than the table allowed, not because of one
   surprise, so the same factor is applied across the board, and phase 2
   also takes on the deferred `cereal query` C support (item 8):

   | Phase | Old estimate | New estimate (source lines, tests and goldens excluded) |
   |---|---|---|
   | 2 References and highlight, plus query C support | ~80 | ~250 (features.c ~90, shared event selection in csymidx.c ~60, query ~90, server ~10) |
   | 3 Hover | ~120 | ~300 (`dump_decl` to a StrBuf touches every caller; record layout) |
   | 4 Rename | ~200 | ~500 (scope tree and conflict check were the least specified part) |
   | Remaining total | ~400 | **~1,050**, so B2 overall about 2,400 lines |

   Actuals are recorded per phase below.
8. **Deferred from phase 1:** `cereal query` C support
   (`tests/query/csym.cmd`, test plan item 3) and `--verify-symbols`
   over `bench/corpus.py` (only zstd.c and cexpr.c were run). Hover
   strings are not stored yet (phase 3). Both items were done since:
   the corpus run in Round 188, the query support in phase 2.

## Phase 2 results and corrections (Round 190)

Phase 2 is done (HISTORY.md Round 190; LSP.md "C symbol index"). Where
it departs from or settles the text above:

1. **Body tokens are not uses** (corrects section 6, which reported
   MACRO_BODY events once at their spelling). references and
   documentHighlight leave them out: the place is the `#define` body, not
   an invocation, and every expansion of the body would point there.
   Tokens written in a macro argument (ARG) count at their spelling, once
   however often the parameter is expanded (the events are deduplicated
   at finish). Pasted and synthesized names (AT_EXPANSION, placed at the
   invocation) count. definition and declaration still answer body
   events (a statement-expression macro's local is declared there).
2. **One location per place.** `cindex_select` (csymidx.c, shared by the
   server and `cereal query`) returns event indices in (file, offset)
   order, one per location, preferring a DECL or DEF event to a REF
   there (highlight shows Write). Several decls at one place (a body
   token, one decl per expansion) therefore give one location; the query
   prints a count line (`2 C entities here`). Results are no longer
   grouped decl by decl as phase 1's definition was; for one decl the
   order is the same.
3. **D1** covers references and documentHighlight too (the same wait
   condition as phase 1, item 1). `tests/lsp/csym_refs` fails without it.
4. **Highlight for macros** (no C entity): the macro definitions are
   Write, every other reference in the requested file Read.
5. **`cereal query`** gains `decl`, `uses` (references without
   declarations: the command-line form of `includeDeclaration: false`)
   and `highlight`; `def`, `decl`, `refs`, `uses` and `highlight` answer
   C names with the section 6 merge, from a second, checking run over the
   unit (only when the macro answer is weak or empty). Hover and rename
   on the command line stay macro-only until phases 3 and 4.
6. **Shared helpers moved into csymidx.c:** `cindex_decls_at`,
   `cindex_validate` (was server.c's) and `cindex_srcfile` (was the
   server's `snap_file` and the dump's `file_by_path`).
7. **Line count:** about 290 lines added and 100 removed (net +190):
   csymidx.c/h 128, features.c 87 (68 replaced), main.c 68, server.c and
   lsp.h 9 (24 removed). Against the re-estimate of ~250 written, about
   15% over; against the original ~80, 3.6x. Phases 3 and 4 keep their
   re-estimates (~300 and ~500).

## Phase 3 results and corrections (Round 192)

Phase 3 is done (HISTORY.md Round 192; LSP.md "C symbol index"). Where
it departs from or settles the text above:

1. **The text** is the `--dump-types` line, from one formatter
   (`cdecl_decl_line`, which `dump_decl` now calls; enumerators through
   `crecord_enumconst_line`): `func NAME: TYPE [static] [extern]
   [inline] [defined]`, `var [f:]NAME: TYPE [static] [extern]
   [tentative]`, `typedef NAME = TYPE`, `enumconst NAME = VALUE (TYPE)`.
   Added for the index: `param NAME: TYPE [register]` (no function
   prefix: a parameter's function is plain), `field NAME: TYPE (struct
   S, offset N[ bit B, width W])`, `label f:NAME`, a struct or union's
   `type_dump_record` layout and an enum's `enum E (underlying T)` with
   one `NAME = VALUE` line per enumerator. Types print as gcc's
   diagnostics spell them (`int(int,  int)`: two spaces after a
   parameter ending in a specifier word). `tentative` is now printed
   only for an object that no declaration initialized (the merged flags
   are ORed, so `int x; int x = 1;` printed it before; no golden
   changed).
2. **When it is copied.** Block-scope symbols at unit end (before the
   lsyms reset), labels at their first event, enums when completed (new
   hook `csx_enum`, the enumerators are only in hand there), and
   persistent symbols, records and fields in `csx_finish`. Doc comments
   are not shown: the lexer drops comments, so they are not trivially
   available.
3. **Interning.** Every text goes through one hash set over the
   builder's string buffer, which becomes the start of the CIndex pool
   (names and paths follow), so equal texts share one offset; hover
   deduplicates decls at one place by that offset.
4. **Bounds.** A text is cut at `CIX_HOVER_MAX` = 1024 bytes (at a
   character boundary, then `...`); a record or enum lists at most
   `CIX_HOVER_MEMBERS` = 16 members, then `... N more`. The worst case
   is many short distinct texts, not long ones: about 7 retained bytes
   per source byte (a 4 MiB struct of `int fN;` members: 29.3 MB with
   330,869 fields; 4 MiB of `int vN;`: 23.3 MB; 74k prototypes with four
   parameters each: 25.4 MB), under the ~30 MB budget, so no shorter
   cap was needed. Real units add 30-39 B per decl (zstd.c 12,717 decls:
   583 KB of texts, 7,416 distinct, 429 KB kept). Time: zstd.c with
   `--verify-symbols`, best of 9 in two alternating rounds, 0.166 and
   0.154 s before against 0.168 and 0.167 s after (at most 8%, about the
   noise between rounds); `cereal check` without the index is unchanged.
5. **Merge** as section 6: a strong macro answer stands; a weak one
   (a name with only macro history) gives the C text plus "(also a
   macro name)". Several decls at one place show their distinct texts,
   at most 5, then "and N more". Hover waits like the other requests
   (D1); `tests/lsp/csym_hover` fails without the wait.
6. **`--dump-symbols`** prints each decl's text after ` :: ` (newlines
   as `\n`), so the `tests/symidx` goldens cover every kind.
7. **Line count:** 349 lines added and 66 removed (net +283):
   csymidx.c/h 253, cdecl.c, cdecl_int.h and crecord.c 59 (48 replaced),
   features.c 19, main.c 16, server.c 2. Against the re-estimate of ~300, about 15%
   over (net slightly under); against the original ~120, 2.9x.
