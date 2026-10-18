---
status: draft
---

# Macro invocation sites in references (Q7) (plan)

Written 2026-10-17. Q7 of [B5_DESIGN.md](B5_DESIGN.md) section 17 ("resolved: keep", parked in 18): `lua_call(L, 0, 0)`
expands to `lua_callk(...)`; clangd lists each `lua_call` as a reference to `lua_callk`, cereal lists none.
clangd's results on lua54 and libuv were 3,509, of which 459 (13%) were such sites (P2 in `clangd_diff.py`);
those are all functions because the differential queries only functions (S1, section 6a).
This plan decides how to list them. Nothing is built; the choice between the two designs waits for spikes.

## 1. What cereal does today (verified at HEAD 04c160e, 2026-10-17, lua54)

| Request | Result for `lua_callk` | Why |
|---|---|---|
| `query callers lua.h:292:16 lauxlib.c` | `luaL_callmeta: 881:3` and `luaL_requiref: 991:5`, the spellings of `lua_call` | call hierarchy reads the unit's **xcalls** and the records' CALL entries flagged `AT_EXPANSION` (`call 881:3 +25723 len 8 ... expansion`) |
| `query refs lua.h:292:16 lauxlib.c` | the DECL only | xcalls live in `ix->xcalls`, apart from `ix->ev` (`csymidx.c:1171-1218`); the body token is `MACRO_BODY` and `cindex_select` skips it (`csymidx.c:1551`); `pq_entries` skips it too (`projq.c:417`) |
| record of `lua.h` | `ref 294:26 +8202 len 9 func lua_callk ext body call` | the body token is in REFS, flagged `MACRO_BODY` |

So **call hierarchy already lists the invocation sites; references, uses and highlights do not**, and the
information for functions is in the records already. xcalls exist for functions only (invariant,
`csymidx.c:2114`).

What kinds of entity have a macro-body use (distinct body-token lines in the records, 2026-10-17):

| | func call | func no call | obj | field | typedef | struct/union | enumconst |
|---|---|---|---|---|---|---|---|
| lua54 | 129 | 0 | 13 | 223 | 131 | 2 | 11 |
| libuv | 14 | 2 | 0 | 40 | 46 | 32 | 25 |

Functions in bodies are almost all calls, so the function case is covered by data we have. The other kinds
(fields such as `->len` in an accessor macro, typedefs, enumerators) are most of the body tokens, and clangd
lists their invocations too. cereal has no invocation record for them.

## 2. Decisions needed from the user

- **D1. Do references list the body token itself** (the `lua_callk` in `#define lua_call(...)`)? **Open
  (reopened 2026-10-17).** The earlier "yes" rested on "clangd does"; that was false: the B differential shows
  clangd lists the invocations, not macro-body tokens (45 of 48 new cereal-only sites were body tokens). So the
  only case left is navigation and rename awareness: the body token is a real spelling the user can go to, but
  a client that reuses the list for a text replace could edit the macro (cereal's own rename refuses, R7). The
  code is not built (`cindex_select` still filters `MACRO_BODY`). Default if undecided: no.
- **D2. Which entities:** functions only (design A) or all kinds (design B). **Decided B (2026-10-17)**, after
  S1 (A covers a third of the sites) and S2 (B costs 2.9 to 5.1%); the size rule of section 5 is relaxed from 5%
  to 10%, the rule CONFIG_DESIGN 8 uses. Design C (macro-pair entries) stays a measured alternative, not chosen.

## 3. Designs

- **A, functions only, no format bump.** `references`, `uses` and `documentHighlight` add: the unit's xcalls and
  the project's CALL entries with `AT_EXPANSION` whose callee is the key, at their `off`/`len`; and, per D1,
  the `MACRO_BODY` refs. Cost: a few dozen lines in `c_collect` and `pq_entries`. Covers a function's calls
  through macros; not a function used as a value in a macro body (2 of 16 on libuv).
- **B, all kinds, no record layout change (S3, section 6a).** Every body-token event gets a second event at the outermost invocation:
  a REF with `AT_EXPANSION` at the macro name's `off`/`len`, one per (invocation, entity). xcalls and their
  CALL entries stay for call hierarchy. The reader, `pq_entries` and the unit query list them; rename keeps its
  R7 refusal because these REFs carry `AT_EXPANSION` (`cindex_rename_blocker`).
- **Not additive:** A's xcall-to-reference code is replaced by B for functions. Per the working rules the
  end-state design wins even though it is larger, if the measurements below say B is affordable. Choosing A
  first is only sensible if B is dropped for good.

## 4. Risks

| Risk | Handling |
|---|---|
| A reference whose text does not contain the name (`lua_call` for `lua_callk`) confuses a client | the same as clangd; the title of the reference is the invocation; documented in LSP.md |
| One invocation, several body tokens of one entity (`#define SWAP(a,b) ... x ... x`) | one location per (invocation, entity), de-duplicated by offset |
| Nested macros (`A` expands to `B(x)` which uses `lua_callk`) | the position is the **outermost** invocation, as `pres` does for xcalls today |
| Result lists grow: the cap is `PQ_MAX_ENTRIES` (20,000) | B adds up to the invocation count of the macros; measured in S2; the cap's refusal message already exists |
| Store growth (B) | S2 measures it; stop rule below |
| Rename sees a new kind of blocker | no: R7 already refuses on `AT_EXPANSION`; the message stays "formed by ## or a macro" and gains the invocation place |
| `symcov` / `--verify-symbols` count the new events as unindexed or extra | they check AST nodes for a missing event, never an event for a missing node, so nothing to excuse (S3, section 6a); `cindex_verify` gets a B invariant instead |

## 5. Stop rules and gates

- **Function sites (A or B):** the P2 class of `clangd_diff.py` project mode, restricted to functions, falls to 0
  on lua54 and libuv; no new unexpected difference.
- **B only:** store growth at most 10% (was 5%; measured 2.9 to 5.1%) on lua54, libuv and zstd lib (the same corpora as S3a), and P2 over all
  kinds falls to 0 or to a listed residue.
- The gate (`gate.sh`): verify, san and callgrind unchanged within 1%; `symcov` 0 unindexed.

## 6. Spikes before any code (S1 to S3 done 2026-10-17, results in 6a; S3 by reading only)

- **S1, split P2 by entity kind.** Run the project differential (`clangd_diff.py --project=lua54` and `libuv`,
  clangd is at `/usr/bin/clangd` in WSL) through `run.ps1`, and print each P2 miss with the kind of the entity
  it references. This says how much of the 13% is functions (design A) and how much is the rest.
- **S2, size of B.** Count the events B would add: per unit, the macro expansions whose body mentions an
  indexed entity, de-duplicated per (invocation, entity), then per record, as a fraction of the store, with the
  S3a method (a throwaway copy). Report events, bytes and the largest result list.
- **S3, the verifier.** Where `--verify-symbols` excuses xcalls, and what B's REFs would need.

### 6a. S1 and S2 results (2026-10-17, WSL, commit 0b8a103 plus this plan edit; runs in `docs/runs.md`)

**S1, P2 by entity kind** (`s1-p2-kinds`, `s1b-body-kinds`; `clangd_diff.py` project mode wrapped to print the
kind of each queried symbol; the wrapper is saved in the run folder).

- The standing differential asks only about top-level functions and variables, so its P2 class (437 + 1 on
  lua54, 21 on libuv = the 459) is **functions by construction**. It cannot say anything about other kinds.
- S1b adds the other kinds, chosen where macro bodies use them: fields, typedefs and enums from clangd's
  `documentSymbol`, plus identifiers in header `#define` bodies (after `->`/`.`, and cast types). Sites are
  counted per (queried entity, site), the unit B would store:

  | | function | field | typedef/class | enum | P2 of all clangd results |
  |---|---|---|---|---|---|
  | lua54 | 960 | 1,422 | 543 | 17 | 2,943 of 9,083 (32%) |
  | libuv | 129 | 26 | 147 | 126 | 428 of 5,090 (8%) |

  So **functions are 30 to 33% of the missing sites** among entities used in macro bodies. Design A closes a
  third; the rest is fields, types and enumerators. The sample is biased toward entities used in macros (that is
  the point), so the percentages are not project-wide.
- Not Q7: S1b's "unexplained" lines are cereal-only sites in files clangd's index missed (clangd's known
  nondeterminism) and 2 `declaration` queries at a field name (harness artefact).

**S2, size of B** (`s2-exported-only`; throwaway probe in `csymidx.c`'s finish step: every body token of a
macro expansion, one entry per distinct (file, offset of the outermost invocation, name), only for declarations
that get a record entry: not locals, parameters or system declarations; 12 bytes each, the `CRecRef` size;
the baseline is the records' bytes from the S3a probe).

| project | records | record bytes | B adds | % | of which function | field | typedef | enumconst |
|---|---|---|---|---|---|---|---|---|
| lua54 | 215 | 1,395,816 | 5,884 | **5.06** | 876 | 3,662 | 1,018 | 41 |
| libuv | 73 | 502,896 | 1,922 | **4.59** | 169 | 1,286 | 163 | 258 |
| zstd 1.5.7 lib | 93 | 810,600 | 1,960 | **2.90** | 1,091 | 379 | 250 | 163 |

- Counting locals, parameters and system declarations (`errno`, `size_t`) first gave 6.2 to 6.8%; they have no
  record entry, so the filter is right (first pass `s2-design-b-size`, superseded).
- **Against the stop rule (5%):** lua54 is 0.06 points over, libuv and zstd are under. The function entries (876,
  169, 1,091) repeat what the xcall CALL entries already hold; without them lua54 is 4.3%, libuv 4.2%.
- Largest single result list B adds: `tt_` +488 (lua54), `loop` +97 (libuv), `_force_has_format_string` +488
  (zstd); far below `PQ_MAX_ENTRIES`.
- **Design C measured as a by-product** (`s2-macro-pairs`): one entry per (outermost invocation, macro) instead
  of per (invocation, entity), with the entity found by joining the macro's body refs at query time:
  4,857 (4.18%), 558 (1.33%), 1,557 (2.30%). It saves 17%, 71% and 21% of B, needs a transitive join for
  nested macros, and gives macro identity (parked Q1 and the macro-rename plan would use it). Not spiked beyond
  the count.
- Not measured: records differ per unit when a header's macro state differs (the count is distinct over all
  units, so it is an estimate), the string pool (no new keys: each body token's key is already stored), and
  S3 (the verifier).

**S3, the verifier and the consumers (read, not run; code at HEAD 0b8a103):**

- `--verify-symbols` (`csx_unit_end`) walks AST nodes that name an entity and reports those without a token
  event ("unindexed"); `symcov` only sums that count. B adds events but no AST node and does not touch `seen`, so
  neither can flag them, and **there is no xcall excuse to extend** (xcalls are simply outside the check; the
  risk-table row of section 4 was wrong). B needs its own invariant in `cindex_verify` instead: every B event is a
  REF with `AT_EXPANSION` whose text at `off` is a macro name different from the entity's. Order and duplicates are
  already checked there (`csymidx.c:2100`); the `ev_cmp` merge in the build (`csymidx.c:1177`) removes the
  repeated-argument duplicate (`SWAP(a,b)` using `x` twice).
- **No layout change, so no `CREC_FORMAT` bump.** `RefRow.flags` already carries `AT_EXPANSION`
  (`cidxrec.c:639`) and `CRecRef.flags` stores it; the build id is a hash of the executable
  (`crec_build_id`), so every new binary rejects old records anyway. This corrects the PARKED rows that said a bump
  is needed.
- `cindex_select` filters only `MACRO_BODY` (`csymidx.c:1551`), so references and uses list B's events with no
  reader change; semantic tokens and inlay hints already skip `AT_EXPANSION` (`features.c:1339,2885`).
- Position lookups at an invocation (hover, definition, prepareRename): `c_decls_at` returns nothing when the
  token is a strong macro target (`features.c:112`), so the macro keeps winning there. Read, not run: the first
  test of B must hover and rename on `lua_call(...)` to confirm.
- Not checked: the project-side readers (`projq.c`, `pq_entries`) for an assumption that a REF's text is the
  entity's name (`pq_ambiguous` compares text at the same offset), and `documentHighlight`.

**What this says for D2:** A is not enough for the question Q7 asks (only a third of the sites), and B is at
the size rule's edge. The choice is now between B as drafted (5.06% at worst) and C (smaller on two of three
projects, one more concept). **The user chose B** (D2); the build order is: the writer's second event per
(invocation, entity), the `cindex_verify` invariant, the references/uses/highlight results, then the tests
(including hover and rename on an invocation, and the clangd P2 class falling to 0).

## 7. Not in this plan

Rename through macros (parked, [MACRO_RENAME_PLAN.md](MACRO_RENAME_PLAN.md)); macro identity and references to
macros across units (Q1); declarations formed by a macro (`PROTO(zm)`); the `#if` side of the macro-aware
index.

## 8. Review log

Three adversarial rounds on 2026-10-17, claims checked in code and by queries on lua54.

- **Round 1** (is the premise true?): the PARKED row says listing needs a format bump. A query showed call
  hierarchy already lists the invocation sites, so the premise holds only for references and for non-call
  entities. The plan is split into A (no bump, functions) and B (bump, all kinds), with the lua54 and libuv
  kind table as the reason.
- **Round 2** (contradictions): the first draft recommended A "because it is cheap" while section 3 shows B
  replaces A's code; the working rules prefer the end-state design, so the plan now says A is only for the case
  where B is dropped, and puts the choice behind S1 and S2 instead of recommending it blind. The D1 question
  (the body token) was hidden inside A; it is its own decision because it also applies to B.
- **Round 3** (what could be false): `uses` printed nothing for `lua_callk` because the unit has no direct use;
  that is consistent with xcalls being outside `ev`. The kind table counts body-token lines, not entities or
  invocations, so it is a proxy; S1 and S2 replace it. The claim that rename is unaffected (R7 refuses on
  `AT_EXPANSION`) was checked against `cindex_rename_blocker` (`csymidx.h:177`) but not run; the rename tests
  of B must include it. The xcall invariant (functions only) is from the verifier at `csymidx.c:2114`.
- **Round 4** (after S1 and S2, claims checked against the run output): the "13% of clangd's results" figure
  was functions only because of the query set, which round 1 had not seen; S1b replaces it with a per-kind
  table. The first S2 pass counted locals, parameters and system declarations that have no record entry
  (6.2 to 6.8%); the probe now skips them (`cidxrec.c:589,633`: only exported declarations are recorded), and a
  rerun that was meant to apply the filter had silently not (the edit was rejected), so its row is marked
  ignore in `runs.md`. Stated limits are in 6a (unit-variant records, string pool, verifier). The 12 bytes per
  entry is the `CRecRef` size; it ignores the section directory and the sorted merge.
- **Round 5** (after building B, clangd differential `q7-b-clangd-diff2`): D1's premise was wrong (see D1) and
  its code is reverted. Presentation differs where a macro call sits in another macro's argument: cereal
  presents at the outermost invocation (gcc's expansion point), clangd at the inner spelling; kept, class P6 in
  `clangd_diff.py`. Result: 0 unexplained clangd misses on lua54 (2632) and libuv (773). The 15 lua54
  "unexpected" cereal-only lines are plain calls outside any macro (`lgc.c:1561` `luaS_clearcache(g);` and so
  on, all in lgc.c, llex.c, ltm.c), so B does not produce them; read as clangd's background index lagging, not
  re-run against a settled index. Gate (`q7-b-gate`): 2211 pass, san 0, verify 2/1/0 differ, callgrind 3.89G,
  all at baseline.
