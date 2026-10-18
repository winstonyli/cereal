---
status: draft
---

# S3: presence conditions in the project index (plan)

Written 2026-10-17. Slice S3 of [CONFIG_DESIGN.md](CONFIG_DESIGN.md) (sections 5 and 8): `CREC_FORMAT` 5,
a Local PC id on each DECL, REF and CALL event, edge PCs in manifests. S1, S2 and S4 are closed without it.
This plan makes S3 a **decision-gated** slice: a spike with no format change runs first, and the format
bump happens only if its numbers pass the stop rule (section 4).

## 1. What S3 would be for

Today a presence condition (PC) exists only while a unit is folded in memory (`analysis_fold`, 33 to 57 ms per
unit, hover and the rename refusal). The records the project index stores carry none. What a stored PC
would answer **without folding the unit**:

- references, call hierarchy and workspace symbol results annotated with the condition that guards each
  site ("under `defined(FEATURE_X)`");
- "is this site ever compiled by any indexed unit?" from the manifest plus the event's id, for a closed file;
- a cheap pre-filter for the rename gate: a use whose id is the trivial PC needs no inactive-group check.

What it would **not** answer, and why (adversarial round 1): the full PC of an event in a unit is the Local PC
composed with the edge PCs along the include chain and the unit's derived formulas (CONFIG_DESIGN 5). The
derived formulas depend on the unit's macro state and are rebuilt by re-skeletonizing each file of the
instance tree, which is a fold. So `sat`, `enable` and `cover` on a stored event still cost a fold of that
one unit; only **display and pre-filtering** are free. No current feature is blocked on S3. That is the reason
for the gate.

## 2. Design (restating CONFIG_DESIGN 5, with the corrections from review)

- **Local PC of an event:** the conjunction, along the nesting of conditional groups of its file, of each
  enclosing group's condition (`#elif`/`#else` arms also conjoin the negation of the earlier arms). It is a
  function of the file text alone, so records stay content-addressed and units that see a header alike still
  write byte-identical records.
- **Canonical form:** a normalized DAG over predicates, children sorted, `and`/`or`/`not` flattened: no BDD, so
  no variable order enters the bytes. Predicates are the atom kinds of `pcexpr.h`: `defined(X)`, `X >= c`,
  family leaves, `HAS` keys, opaque text.
- **What the id means** (round 1): the predicates are over the *initial* value of each macro (the default
  environment, `pcexpr.h`). A condition that tests a macro the same file defined earlier is therefore not
  the same as its meaning in the unit. The id names the condition's **syntactic form**, not its
  satisfiability; consumers must not call `sat` on it without the fold. The records say so (field name
  `lpc`, not `pc`), and the doc of every consumer repeats it.
- **Storage:** parallel arrays, so no existing entry changes layout: `DPC`, `RPC`, `CPC` (one `u32` index per
  event of DECL, REF, CALL) into one `PCT` table of normalized bodies per record. Index 0 is "true" (no
  enclosing group), which makes the common case one zero per event and compressible by the section layout.
- **Event to group:** a per-file table from byte offset to innermost group, built once per skeleton. Today's
  `group_at` is a linear scan per query; per-event assignment needs the table (a sweep over the sorted events).
- **Manifests (split from records, round 2):** the inclusion-instance tree with edge PCs and untaken edges needs
  the fold of every unit at index time (today only `cover_max > 0` folds, `cidxrec.c`). That is the dominant
  cost, `pc_fold` being 37% of an `lj_state.c` profile. **S3 is split:** S3-records (Local PC ids, skeleton
  only, cheap) and S3-manifests (edge PCs and instance trees, a fold per unit). S3-manifests is parked until a
  consumer needs an edge PC (PARKED.md entry on adding).

## 3. Risks

| Risk | Handling |
|---|---|
| Store grows more than the 10% rule | measured first (S3a); events with the trivial PC cost one zero |
| Every cache invalidated once (`CREC_FORMAT` is in the build seed) | intended (CONFIG_DESIGN 5); one cold index after the bump |
| An id computed from a skeleton in a header whose text a unit reads differently (`#include` order, `-include`) | the id depends on the file text only; the unit-dependent part stays in the fold |
| Two spellings of one condition get two ids (`defined(X)` vs `defined X`, `!(A&&B)` vs `!A||!B`) | normalization is syntactic (flatten, sort, no De Morgan); extra ids cost bytes, never correctness; measured in S3a |
| The reader must reject a bad `PCT` (offsets, cycles) | the existing whole-file validation in `crec_parse` extends to the table; a fuzz case joins the corrupt-record tests |
| Dump/golden churn (`crec_dump`, 14.2) | the dump prints `lpc` as normalized text; goldens regenerate once |

## 4. Stages and stop rules

**S3a, the spike (no format change, a throwaway tool).** For libuv, lua54 and zstd: for each record file the
index writes, compute the Local PC id of every event (skeleton plus a sweep), then report: events,
fraction with a non-trivial id, distinct ids per record, estimated added bytes (`4 * events + table`)
against the current record bytes of the store, and the id's stability (the same header's records gain the
same ids across units). Stop rule: **estimated growth at most 10% of the store** (CONFIG_DESIGN 8) **and** a
named consumer in section 1 whose output is visibly better with the ids. If growth is under the rule but no
consumer is chosen, S3 stays parked (PARKED.md) with these numbers as evidence.

**S3b, records.** Format 5: `DPC`/`RPC`/`CPC` and `PCT`; writer in `crec_index` (skeleton only, no fold);
reader and validation; `crec_dump` prints the normalized text. Gate: `symcov` unchanged; store size as
measured; goldens regenerated and reviewed; the whole suite; the bad-table cases.

**S3c, one consumer.** Chosen from section 1 after S3a; its own test. Candidates in order of cost: references
annotation (`lsp/project.c` queries already read records), then the rename pre-filter.

**S3-manifests.** Parked until a consumer needs an edge PC.

## 5. Not in this plan

Target and annotation fields (S5), the overlay view, cross-unit `cover` from stored PCs, anything that changes
the fold. The in-memory overlay variant of the enable action is independent of S3 (PARKED.md).

## 6. Review log

- **Round 1** (claims against the code): the doc's "queries on stored PCs are cheap" fails for `sat`/`enable`
  because derived formulas need the fold; section 1 now states what is free. The initial-value environment
  makes the id syntactic; section 2 says so and renames the field.
- **Round 2** (cost): edge PCs and instance trees cost a fold per unit at index time, which today is paid
  only with `cover_max`; section 2 splits S3-records from S3-manifests.
- **Round 3** (what could be false): "records stay byte-identical across units" holds for the ids only because
  they depend on file text alone; recorded as a gate in S3a (stability check). Checked in `cidxrec.c`:
  events are sorted and equal neighbours merged (`sort_unique`), with offsets as the sort key
  (`decl_cmp`, `ref_cmp`, `call_cmp`, `macro_cmp` all compare `off` first). The ids are therefore computed
  **after** `sort_unique`, from each event's `off`, so the parallel arrays stay aligned by construction;
  two events at one offset share a group, so merging cannot split them. Verified 2026-10-17; the spike
  re-checks it on the corpus.
