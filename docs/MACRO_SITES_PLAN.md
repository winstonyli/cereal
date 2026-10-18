---
status: draft
---

# Macro invocation sites in references (Q7) (plan)

Written 2026-10-17. Q7 of [B5_DESIGN.md](B5_DESIGN.md) section 17 ("resolved: keep", parked in 18): `lua_call(L, 0, 0)`
expands to `lua_callk(...)`; clangd lists each `lua_call` as a reference to `lua_callk`, cereal lists none.
clangd's results on lua54 and libuv were 3,425, of which 459 (13%) were such sites (P2 in `clangd_diff.py`).
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

- **D1. Do references list the body token itself** (the `lua_callk` in `#define lua_call(...)`)? clangd does. It
  is a real spelling the user can navigate to and edit. Recommendation: yes, it needs no new data.
- **D2. Which entities:** functions only (design A) or all kinds (design B).

## 3. Designs

- **A, functions only, no format bump.** `references`, `uses` and `documentHighlight` add: the unit's xcalls and
  the project's CALL entries with `AT_EXPANSION` whose callee is the key, at their `off`/`len`; and, per D1,
  the `MACRO_BODY` refs. Cost: a few dozen lines in `c_collect` and `pq_entries`. Covers a function's calls
  through macros; not a function used as a value in a macro body (2 of 16 on libuv).
- **B, all kinds, record format 5.** Every body-token event gets a second event at the outermost invocation:
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
| `symcov` / `--verify-symbols` count the new events as unindexed or extra | they run on identifier tokens in source text; an invocation spelled `lua_call` is not an occurrence of `lua_callk`, so the verifier must excuse it: the existing excuse for xcalls extends to B's REFs (checked in S3) |

## 5. Stop rules and gates

- **Function sites (A or B):** the P2 class of `clangd_diff.py` project mode, restricted to functions, falls to 0
  on lua54 and libuv; no new unexpected difference.
- **B only:** store growth at most 5% on lua54, libuv and zstd lib (the same corpora as S3a), and P2 over all
  kinds falls to 0 or to a listed residue.
- The gate (`gate.sh`): verify, san and callgrind unchanged within 1%; `symcov` 0 unindexed.

## 6. Spikes before any code

- **S1, split P2 by entity kind.** Run the project differential (`clangd_diff.py --project=lua54` and `libuv`,
  clangd is at `/usr/bin/clangd` in WSL) through `run.ps1`, and print each P2 miss with the kind of the entity
  it references. This says how much of the 13% is functions (design A) and how much is the rest.
- **S2, size of B.** Count the events B would add: per unit, the macro expansions whose body mentions an
  indexed entity, de-duplicated per (invocation, entity), then per record, as a fraction of the store, with the
  S3a method (a throwaway copy). Report events, bytes and the largest result list.
- **S3, the verifier.** Where `--verify-symbols` excuses xcalls, and what B's REFs would need.

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
