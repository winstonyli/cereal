---
status: done
---

# Attribute names as ids in checker state, implementation plan (AR4 stage 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (inline, one implementer). Steps use checkbox (`- [ ]`) syntax.

**Goal:** `AName`, `AttrState.cur` and `Checker.ign` hold `AttrId`, `attr_excl_tab` uses enumerators, and the guard passes with an empty allowlist; output is byte-identical.

**Architecture:** `attrs.h` gains `AT_NONE`/`AT_UNKNOWN` bracketing the row ids and three lookups (`attr_by_id`, `attr_name`, `attr_id_of`); then the set API and the two state arrays switch from strings to ids in one atomic change (a `const char *` passed for an `AttrId` is an `-Werror=int-conversion` error here but `%s` of an id compiles silently, so no half-converted tree may land).

**Tech Stack:** C99 (`src/c/`), sh/awk guard (`bench/tools/`), the WSL gate (`docs/S1_PLAN.md` "Closing a slice").

**Spec:** `docs/ATTR_IDS_DESIGN.md` (read it first: "Behaviour that must be preserved" binds every task).

## Global Constraints

- Output identical to the pre-task tree on the gate; no existing `.expected` changes; the only new goldens are Task 1's four (cases a-d), whose expected files are recorded from the unmodified binary.
- Quick loop in WSL: `wsl -e sh -c 'sh ~/cereal-s1/.sync.sh'` (rsync + make), then `sh tests/run.sh` there. Gate, detached with a watcher: `powershell -NoProfile -File "$USERPROFILE/.claude/runs/run.ps1" -Kind gate -Name <n> -Detach -Command "wsl -e sh /home/winstonyli/rungate.sh"`.
- Gate expectations: run/san `N passed, 0 failed` (N = pre-change count + 8: `par_goldens` runs each case twice, plain and `--cells`), san `findings 0`, verify and globals as on the baseline gate recorded in Task 1, callgrind `Collected` within ±0.2% of that baseline (stage 1 moved −0.10%).
- `AC_*`, `AF_*` and all `attrs.def` rows untouched; rows keep the C-locale `strcmp` order.
- `AttrId` values are internal: nothing writes them to a digest, summary or index (spec "Docs to update": grepped `csum`, `cdigest`, `cidx`); Task 4 re-greps.
- Python never reads stdin; scratch scripts go in the scratchpad, never the repo.
- No commit pushes, tags or bumps.

## Review Focus

1. A typedef or declaration with only an unknown attribute still makes a set that fills a `cexpr_asets` slot: `has_attribute` on the 3-slot setup stays 0 (golden a).
2. 24 unknown attributes then `pure` then `noreturn`: the 24 cap still drops `pure`'s push, so no conflict is printed (golden b).
3. A tombstoned (`packed`-dropped) set is still copied by `copy(v)` and still fills a slot (golden c).
4. A differing `alloc_size` redeclaration is still ignored by `aset_add` via `ign` (golden d).
5. `copy_excluded` on `AT_NONE`/`AT_UNKNOWN` entries returns false without dereferencing (covered by goldens a and c running under the san stage).

## End-state fit

Home: `attrs.h`/`attrs.def` own ids and names, `cattr.c` owns the sets, `attr_by_id`/`attr_name`/`attr_id_of` and the exclusion table. Replaces `AName.name`, `AttrState.cur[24][24]`, `Checker.ign[8][24]`, the string-typed `aset_*`/`cdecl_aset_*` signatures, `AT_COUNT_`, and the `attr_excl_tab` allowlist line. Deletes the empty-string tombstone spelling (it becomes an `AT_NONE` entry, still kept). Nothing is kept beside the new representation; stage 3 (`pure`/`cnst`, `Attrs` flags) and handler dispatch stay parked.

---

### Task 1: baseline goldens

**Files:** Create `tests/check/attr_ids_{unknown_cap,cap_order,tombstone,alloc_ign}.{c,expected}` (names are the builder's; one case per spec golden a-d).

- [ ] **Step 1: Write the four cases** per spec "Verification" (a)-(d). Each: first line `// flags: -std=gnu99 -Wall`, `extern char chk[cond ? 1 : -1]` style assertions as in `tests/check/has_attribute.c`, a comment on (a) and (c) that they pin cereal's 3-slot cap and not gcc's, cross-referencing `docs/ATTR_IDS_DESIGN.md` "Found". (c) is `T2 y __attribute__((copy(v)))` with (a)'s typedef chain, and also queries `copy(x)` of an unknown-only declaration so `copy_excluded` meets an `AT_UNKNOWN` entry (Review Focus 5). Assert the *probed* value, not a guess: write each query as both `YES(...)` and `NO(...)` in scratch, run, read which one errors with a negative array size, then keep the one that compiles.
- [ ] **Step 2: Record from the unmodified binary.** `.sync.sh`, then generate each `.expected` as `check_golden` does (`tests/run.sh:353-367`: `cat stderr stdout` of `cereal -fsyntax-only -std=c99 -pedantic <flags> file.c` from `tests/check`). Confirm (c) really takes the tombstone-only path (a set exists, no live entry) and that each golden discriminates, with one scratch mutation per golden (save `git diff > scratch.patch` first, revert with `git checkout -- src` only after commit state is clean): (a) and (c): skip unknown/tombstone entries in `aset_add`; (b): stop `attr_excl_generic` pushing unknown names into `cur` (`cattr.c:2633`; `aset_add` is not on that path); (d): delete the `ign` loop in `aset_add` (`cattr.c:2391`). Each must make its own golden fail.
- [ ] **Step 3: Run `sh tests/run.sh`.** Expected: `N+8 passed, 0 failed`.
- [ ] **Step 4: Baseline gate**, detached with a watcher (`rungate.sh` always logs to `~/gate_t5.log`, shared and overwritten: copy it per run); record its run id, counts and callgrind total in this plan's "Baseline" line (edit the line below).
- [ ] **Step 5: Commit** `AR4 stage 2: goldens for the set machinery's observable behaviour`; set this plan and the spec to `status: active`.

Baseline: run `2026-11-05-ar4s2-baseline2` (commit 527c358): run 2378/0, san 2377/0 findings 0, verify 637/638, 289/289, 3908/3910, callgrind Collected 3,814,609,402.

### Task 2: ids and lookups, unused

**Files:** Modify `src/c/attrs.h`, `src/c/attrs.def` (header comment), `src/c/cattr.c`.

**Interfaces:**
- Produces: `enum AttrId` with `AT_NONE = 0`, then the rows in order, then `AT_UNKNOWN`; `typedef enum AttrId AttrId;` `AT_COUNT_` deleted. `const AttrDesc *attr_by_id(AttrId)` (NULL for `AT_NONE`/`AT_UNKNOWN`), `const char *attr_name(AttrId)` (`""` / `"?"` for those two), `AttrId attr_id_of(const char *normalised)` (`AT_UNKNOWN` when no row).
- Consumes: `attr_tab`, `attr_lookup`.

- [ ] **Step 1: Add a static check that fails first:** `_Static_assert(sizeof attr_tab / sizeof *attr_tab == AT_UNKNOWN - 1, ...)` replaces the `AT_COUNT_` assert and fails to compile until the enum change is made.
- [ ] **Step 2: Implement** the enum (X-macro with `AT_NONE = 0,` first and `AT_UNKNOWN` last), `attr_by_id` as `attr_tab[id - 1]` guarded for the two non-rows, `attr_name`, `attr_id_of`; `attr_lookup` bisects over `sizeof attr_tab / sizeof *attr_tab`. Update the `attrs.def` header comment (`attr_tab[AT_X - 1]`). Grep `\[AT_`, `attr_tab\[` and every `->id` / `.id` comparison on `AttrDesc` (`cexpr_name.c:1320` compares by enumerator, fine) for any index use of the old values.
- [ ] **Step 3: `.sync.sh` and `sh tests/run.sh`.** Expected: builds clean (`-Wunused` clean: the new functions are `attr_by_id`/`attr_name`/`attr_id_of` non-static, declared in `attrs.h`), `N+8 passed, 0 failed`.
- [ ] **Step 4: Commit** `AR4 stage 2: AT_NONE/AT_UNKNOWN and id lookups (unused)`.

### Task 3: ids in the sets and the state (atomic)

**Files:** Modify `src/c/check_int.h` (`AName`, `Checker.ign` at :359, `cdecl_aset_*` prototypes), `src/c/cdecl_int.h` (`AttrState.cur`, `aset_*` prototypes; `#include "c/attrs.h"` if absent), `src/c/cattr.c`, `src/c/cdecl.c`, `src/c/ccall.c`, `src/c/cexpr_alias.c`, `src/c/cexpr_bi.c`, `src/c/cgx86.c`, `src/c/cmerge.c`, `src/c/crecord.c`, `src/c/cstmt.c`, `src/c/cwarn_expr.c`.

**Interfaces:**
- Produces: `AName { uint32_t set, prev; uint16_t id; char arg[24]; }`; `void aset_add(Checker*, uint32_t *set, AttrId, const char *arg)`; `void aset_drop(Checker*, uint32_t set, AttrId)`; `bool cdecl_aset_has(const Checker*, uint32_t set, AttrId, const char *arg)`; `bool cdecl_aset_first_arg(const Checker*, uint32_t set, AttrId, char *out, size_t n)`; `AttrId cur[24]`; `Checker.ign` as `AttrId ign[8]`; `attr_excl_tab` rows `{AttrId n; AttrId ex[8]; bool fn_only}` (renamed `obj` in AR4 stage 3) ending each `ex` list with `AT_NONE`; `attr_excl_generic(Checker*, uint32_t tok, AttrId, uint32_t kind, AttrState*)`.
- Consumes: Task 2's lookups.

- [ ] **Step 1: Change the types and prototypes first** (`AName`, `cur`, `ign`, the four `aset_*`/`cdecl_aset_*` signatures, `attr_excl_tab`, `attr_excl_generic`) and run `make -k` directly (the Makefile already has `-Werror=int-conversion`; `.sync.sh` pipes make through `head` and stops at the first failing file, so it cannot give the list). Expected: an error at every unconverted site; that list is the work list (spec: the lists in the design are not exhaustive). The compiler misses `%s` sites (`cwarn`/`cwarn_d` have no format attribute, `check_int.h:709,729`): also temporarily add `__attribute__((format(printf, 4, 5)))` there (check the real argument positions), rebuild, and fix each warning by passing `attr_name(id)`; remove the attribute before committing. Known sites: `attr_excl_generic`'s `ia`/`io`/`an`/`o`, `an2[w]` (`cattr.c:2270`), `cdecl_alloc_conflict(name)` callers, `alloc_redecl`'s `name`.
- [ ] **Step 2: Convert every site**, behaviour as the spec's five pinned items:
  - `aset_add` filters `ign` by id and de-duplicates by (id, arg) uniformly (no `AT_UNKNOWN` special case); `aset_drop` sets the entry's id to `AT_NONE`; `aset_keep_max_aligned` matches `AT_ALIGNED` and tombstones to `AT_NONE`; `aset_copy` passes `a.id`; `copy_excluded(AttrId)` is `d = attr_by_id(id); return d && (d->flags & AF_COPY_EXCL)`.
  - Callers holding source text compute the id once after `attr_norm` via `attr_id_of`: `cdecl_attrs_names` (unknown names still `aset_add(AT_UNKNOWN, args)`), `e_has_attr` (keeps `an` for its diagnostics), the attribute loops in `attrs_copy_check`/the alloc and malloc paths (its `an[0] == 'p'` pure/const test and `strcmp(an, "copy")`, and `attrs_alloc_check`'s `name[0] == 'p'` test, become id compares). Literal `"x"` arguments become `AT_X` (spec "Design" lists the sites; `cdecl.c:2650,2663,2669,3516` included). The `an2[]`/`cn[]` arrays become `AttrId` arrays; `alloc_redecl` takes an id and pushes it to `ign`.
  - `ign` pushes (five sites in `cattr.c`) store enumerators and keep the `nign < 8` guard; `cmerge.c:491` compares `AT_NOINLINE`; `aset_opt_args` compares `AT_OPTIMIZE`.
  - `attr_excl_generic`: `cur` pushes `an` up to 24 including `AT_UNKNOWN`; every `strcmp(an|o, "…")` becomes an id compare; the `snprintf(c->ign…)` expression becomes the same ternary on ids.
  - Strings stay where source text is the input (`attrs_item_named`, `attr_arg_eq`, `cdecl_attr_name`, `cdecl_attr_args`); key `attr_arg_eq` by `attr_name(id)` or by id, builder's call, noted in the commit message.
- [ ] **Step 3: Build clean** with the `-Werror` flags above; then `grep -rn 'anames.data\[[^]]*\]\.name\|snprintf(.*ign\|snprintf(.*->cur' src/c` must find nothing.
- [ ] **Step 4: `.sync.sh`, `sh tests/run.sh`.** Expected: `N+8 passed, 0 failed`, Task 1 goldens unchanged.
- [ ] **Step 5: Commit** `AR4 stage 2: attribute ids in the sets, cur and ign`.

### Task 4: guard, docs, gate

**Files:** Modify `bench/tools/attr_lists.allow` (delete the entry), `PARKED.md`, `docs/ATTR_TABLE_DESIGN.md`, `docs/ATTR_TABLE_PLAN.md`, `docs/CLEANUP_PLAN.md`, `docs/STATUS.md:47` (golden count, +4 files), `src/c/attrs.def`/`attrs.h` header comments, code comments named in the spec ("Docs to update").

- [ ] **Step 1: `sh bench/tools/attr_lists.sh`** with the allowlist entry still present. Expected: `allowed … but gone` (the table now holds enumerators), exit 1. Delete the entry; rerun. Expected: `attr lists: ok`, allowlist file holds only its comment.
- [ ] **Step 2: Docs check:** apply every item in the spec's "Docs to update" (split the PARKED stage-2 entry so stage 3 stays; keep the `sets[3]` entry); grep `attr_excl_tab`, `ids never`, `stay strings`, `AT_COUNT_`, `attr_tab\[AT_` across `docs/`, `PARKED.md`, `src/` and fix or report stale lines; re-grep `csum`, `cdigest`, `cidx` for `AttrId`/`anames`. Set the spec to `status: done` and this plan to `done`; append the stage-2 outcome to the CLEANUP_PLAN AR4 item.
- [ ] **Step 3: Full gate**, detached with a watcher. Expected per Global Constraints against the Task 1 baseline.
- [ ] **Step 4: Commit** `AR4 stage 2: empty the attribute-list allowlist, docs`. Then the one whole-branch review (Sonnet, branch under ~300 changed lines) per executing-plans.
