---
status: draft
---

# Project rename through macro bodies (plan)

Written 2026-10-17. **Parked for later by decision (same day); nothing below is scheduled.** Option 2 of the macro-aware rename policy ([PARKED.md](../PARKED.md) "A macro-aware
rename policy"): **edit the body token when it is a whole-word identifier that every expansion resolves to the
renamed entity, and refuse everything else.** No per-site prompt (option 3), no change to what references list
(Q7 stays parked). Nothing here is built; the plan is gated by spikes (section 6).

## 1. Scope

- **In:** the project rename path (`rename_plan` in `lsp/features.c`), for an entity with external linkage
  (`c_external`: a function or object). Today it refuses any use in a `#define` body or at a macro expansion
  (R4/R7). Measured on the records of the corpus (distinct entities with a macro-body event, 2026-10-17):
  libuv 5 external functions (the 5 "macro" refusals of `rename_libuv.py`), lua54 63 external functions and 3
  objects (the API wrappers `lua_call`, `lua_pcall`, ...).
- **Out, keeps refusing:** locals, statics, fields, types, enumerators, tags. A body token that names one of
  them can resolve to another entity at an expansion in a unit the rename does not look at (a macro using a
  caller's local `err`), and a single-unit rename has no project-wide coverage to check. The single-unit path
  (`cindex_rename_plan`, `cereal query rename=`) is unchanged.
- **Out, still refuses:** a name formed by `##`, a `#` operand, or a synthesized name (`PROTO(zm)` making
  `zm_fn`): there is no spelling in the text to edit.

## 2. What is already true (verified against HEAD 345f6eb, 2026-10-17)

- **Records carry the body token.** `lua.h` in the `lauxlib.c` record has
  `ref 294:26 +8202 len 9 func lua_callk ext body call` for `#define lua_call(L,n,r) lua_callk(...)`, plus an
  `expansion` CALL at each invocation (`--dump`). `cidxrec.c:639` keeps `CIX_MACRO_BODY` on REFS. So **no
  `CREC_FORMAT` bump is needed** for the edits (this corrects the PARKED rows, which tied the policy to the
  Q7 bump; Q7, listing invocation sites, still needs one and stays parked).
- **R1 already covers a body position that is another entity elsewhere.** `pq_ambiguous` compares every REF
  and DECL of every record at the same (path, text, offset), so a body token that resolves to `lua_callk` in
  one unit and to a local in another is refused (`projq.c:620`).
- **The unit check is an oracle for the macros the unit expands.** `c_rename` (`crename.c:501`) re-preprocesses
  and re-checks each affected unit with the edits applied and `differs()` refuses any change beyond the name.
- **The gap is the macro nobody expands.** No expansion means no record event and no oracle (R4/R7 note in
  `B5_DESIGN.md` 11.5). That is a *silent miss*, the reason the rule exists.

## 3. Design

- **D1, edit set.** In `pq_entries` a REF with `CIX_MACRO_BODY` is an edit, not a blocker, when the caller asks
  for rename edits for an external function or object. A REF with `CIX_AT_EXPANSION` (pasted, synthesized) is
  still a blocker with today's message. The open unit's events are unchanged: `cindex_select` already leaves
  body events out of `CIQ_REFS` (`csymidx.c:1551`), and the same positions arrive from the records.
- **D2, coverage check (closes the silent miss).** In `c_rename`, for every affected unit, every macro
  *definition* the unit reads (all versions, user files) is scanned: each body identifier spelled `old` that is
  not a parameter must have its spelling location in the edit set, else the rename is refused with
  "the body of macro M at file:line spells 'old', but no unit expands it to this function". A body identifier
  next to `#` or `##` is refused as "formed by ##". Parameters are excluded with `Macro.params` / `TF_PARAM`,
  so `#define F(foo) foo` does not count. Macros in inactive groups are not in the unit's macro table; the
  existing `inactive_use` scan already refuses a word in a skipped range (A3.9).
  **Capture (round 1):** a macro with an edited body token is also refused when the *new* name is one of its
  parameters (`#define F(n) old(n)` renamed to `n` would call the parameter). An expanded macro is caught by
  the post-edit comparison; an unexpanded one only by this check.
- **D3, the oracle stays.** The post-edit unit check of `c_rename` is unchanged and now also covers body edits
  in the macros a unit expands. A unit where an expansion resolves the token to a different entity fails the
  comparison.
- **D4, messages.** The R4/R7 text stays for what is still refused; D2 adds one message. LSP.md and the
  `renameMaxTUs`/R-table in `B5_DESIGN.md` 11.5 are updated to say bodies are edited.

## 4. Risks

| Risk | Handling |
|---|---|
| An invocation spelling edited by mistake (`lua_call` renamed to the new function name) | The invocation event is the xcall (`REF\|CALL\|AT_EXPANSION`, `csymidx.c:2114`), not a spelling of the entity. Edits come only from `MACRO_BODY` refs; tests assert the invocation text is unchanged (spike S1 checks that no xcall enters the unit's edit list) |
| A macro defined twice (variants) or redefined after use | D2 walks every version (`Macro.prev`); the edit set holds one position per text variant, as for any record |
| A system-header macro names the function | records and edits cover user files only; refused if the position is in a file the editor cannot place (R3) |
| A header included by no indexed unit mentions the name in a macro | not reachable: R2 requires every unit indexed; an unindexed header is outside the project's rename scope, as for any other use in it |
| Cost of D2 | a walk over each affected unit's macro table once per unit, inside the check that already preprocesses the unit |
| A body token that resolves to the key in one unit and to nothing (undeclared) in another | the second unit does not compile the macro's expansion with the name; if it expands it, its own check fails today too. If it never expands it, D2 passes it (its position is in the set) |

## 5. Tests and the falsifier

- `tests/lsp` fixture `renmacro/` (a project of three units): (a) a wrapper macro expanded in two units is
  renamed in its body, invocations untouched, the post-check clean; (b) a macro that names the function and is
  never expanded: refused by D2; (c) `#define F(foo) foo` with a function `foo`: not an occurrence, rename
  proceeds; (d) `#define P(n) foo##n`: refused; (e) the same macro expanded where `foo` is a local: refused (R1);
  (f) a macro in an inactive group: refused (A3.9); (g) a local variable renamed: still refused.
- **Falsifier on the corpus (round 2: a compile cannot see an unexpanded macro, so it is not the oracle):** for
  every rename that now succeeds, apply the edits to a copy of the project and, for every unit with its flags,
  compare `cc -E -dD -P` of the renamed copy with `cc -E -dD -P` of the original after a token-level
  substitution `old -> new` (outside string and character literals; a small script in a file, never on stdin).
  `-dD` keeps every `#define`, so an unexpanded macro that was missed shows as a line that still says `old`,
  and a pasted or captured name shows as a different line. Any difference is a false edit or a miss. Extend
  `bench/tools/rename_libuv.py` to lua54. Stop rule: **0 differences**, and every libuv function that is
  refused for a macro today ends either renamed with 0 differences or refused with a reason from D2, the
  `##` rule or R1, listed by name. The run goes through `run.ps1`.
- Gate: `gate.sh` unchanged results (verify, san, callgrind within 1%).

## 6. Spikes before code

- **S1.** Dump the open unit's events for `lua_callk` after `lua_call` is expanded: which enter `CIQ_REFS`, and
  whether an xcall can reach `p->locs`. Expected: none.
- **S2.** Enumerate a unit's macro definitions for D2: the `MacroTab` (by identifier, `prev` chain) against the
  skeleton's `SK_DEFINE` directives. Pick the one that sees redefinitions and costs least; the skeleton also
  sees inactive groups.
- **S3.** Lift the blocker in a scratch copy and run the lua54 rename oracle: count successes and false edits
  before writing the final code.

## 7. Not in this plan

Listing invocation sites of macros in references and call hierarchy (Q7); a rename through a macro *argument*
(already an ordinary reference); fields, types and locals in macro bodies; the single-unit path.

## 8. Review log

Three adversarial rounds on 2026-10-17, claims checked against HEAD 345f6eb and a dump of a lua54 record.

- **Round 1** (what the rule can still get wrong): a *capture* hole in D2 (the new name is a parameter of the
  macro) that only the post-edit comparison catches for expanded macros; added to D2. A fresh `old` typed
  into the open buffer is not in the records, so D2 refuses it (position not in the edit set), which is
  right. `prepareRename` shares the rule and must change with `rename`.
- **Round 2** (the proof): the draft's falsifier compiled the renamed project, which cannot see an unexpanded
  macro, the one case D2 exists for. Replaced by the `cc -E -dD -P` comparison against the substituted
  original, which sees every `#define`.
- **Round 3** (what could be false): checked in code: records keep `MACRO_BODY` on REFS (dump line
  `ref 294:26 ... body call`); `pq_ambiguous` covers body positions; `cindex_select` drops body events
  from `CIQ_REFS` (`csymidx.c:1551`); `pq_locs_sort` dedupes the same body position seen by many records.
  **Not verified, so spikes:** (S1 is resolved by reading: xcalls live in `ix->xcalls`, apart from `ix->ev`,
  `csymidx.c:1171-1218`, so they cannot reach an edit list.) How to
  enumerate macro definitions including redefinitions (S2), and that `differs()` accepts a body edit
  (S3). The PARKED rows that tie the policy to the Q7 format bump are stale for rename and are corrected
  when the work lands, not before.
