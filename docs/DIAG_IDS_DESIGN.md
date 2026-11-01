---
status: done
---

# Diagnostic option ids as an enum generated from one table

Item IN1 of `docs/CLEANUP_PLAN.md` (the f3 part). Direction chosen 2026-11-01 (the user's "h1"): the option table generates an enum, and the diagnostic entry points take the enum. Written for review; nothing is built.

## Problem

Every diagnostic and every "is this warning on?" question names its option by a string: `diag_report(d, lvl, "format=", loc, ...)`, `diag_enabled(d, "undef")`. Facts, counted 2026-11-01 on `daeccfd`:

- `diag.c` has one registry of **187** options (`options[]`, no duplicate names). About **630** string literals outside `diag.c` name an option at a call or in a data table (`ci.warnopt = "builtin-declaration-mismatch"`, `opt = kind == UC_REAL ? "float-conversion" : ...`), through `diag_report`/`diag_vreport` (155 sites) and about 15 forwarding wrappers (`cwarn`, `cwarn_d`, `cpedwarn`, `cpedstd`, `vrep`, `vped`, `iwarn`, `pvreport`, `pwarn_opt`, `pp_warn_at`, `lit.c say/note`, `conv_diag`, `cc90`).
- A misspelled name is not an error. `diag_level_for` finds no index and returns the requested level, so the warning silently ignores `-Wno-...`.
- Each call resolves its name at run time: a 512-entry pointer-keyed cache (`DiagEngine.idc_key/idc_val`, about 5 KB per engine) in front of a bisection over a sorted name table. On `uvloop/loop.c` (`-w`, as the gate runs it) `find_index_cached` costs 64.2M of 3,889.7M instructions (1.65%, inclusive, a work count and not a time).

## Design

**One table, one enum.** The rows move to `src/diag_opts.def`, one `DIAG_OPT(ID, "name", "group", level, on, by, "help")` per option (the same fields as `DiagOption` today). `diag.h` includes it twice:

```c
typedef enum DiagOpt {
    DW_NONE = 0,                 /* hard errors and anything not configurable: id "" */
#define DIAG_OPT(id, ...) DW_##id,
#include "diag_opts.def"
#undef DIAG_OPT
    DW_COUNT
} DiagOpt;
```

`diag.c` builds `options[]` from the same file with the row for `DW_X` at index `DW_X - 1`, so the enum value is the row index and a lookup is an array access. `_Static_assert(sizeof options / sizeof *options == DW_COUNT - 1)`.

Identifiers are derived once by script and then maintained by hand: `DW_` plus the name upper-cased, `-` to `_`, `+` to `P`, a trailing `=` to `_V` (`format=` is `DW_FORMAT_V`, `c++-compat` is `DW_CPP_COMPAT`). The script checks that all 187 derived identifiers are distinct (checked 2026-11-01: they are). The prefix is `DW_` because `W_TARGET` and `W_LANG` are macros in `analysis/pcq.c` and `DO_*` are the umbrella flags.

**Entry points take `DiagOpt`.** `diag_report`, `diag_vreport`, `diag_enabled`, `diag_option_state/explicit/requested/level/size`, and the wrappers listed above. A hard error passes `DW_NONE`. Data-table fields that hold an option name (`warnopt`, `opt`) change type with them. `-Werror=int-conversion` joins `WARN` in the Makefile, so a string left where an enum belongs fails the build instead of warning.

**The string boundary stays.** Names are still text where they come from outside or are stored:

- `Diagnostic.id` stays `const char *` (set to `options[opt - 1].name`, or `""`). The JSON output, the cell encoding (`cell.c`: `cd->id = d->id`), the fix mode records (`fixtu.c:101`, `fixmode.c`) and the `--only=` list are unchanged, so **no cell or stored-format version moves.**
- `DiagOpt diag_opt_find(const char *name)` (new, the old `find_index` as a function of the name) serves `-W` flag parsing, `#pragma GCC diagnostic`, `--only=`, the cell replay and `diag_map_enabled(map, const char *id)`. It keeps the sorted `by_name` table and its `pthread_once`. Unknown name: `DW_NONE`.

**Deleted:** `idc_key`/`idc_val`, `find_index_cached`, the per-call `find_index`; the six `diag_option_*` and `diag_enabled` index lookups become `options[opt - 1]`. **Kept:** `by_name`, `find_index_scan` (aliases and `X=N` forms), the `memo`.

## What this subsumes and what it cannot

Subsumes: the id cache, the per-call name search, and the class of "misspelled option name" bugs (a wrong identifier does not compile). Cannot: `gcc_wopts.h` (150 names: gcc's own option list, used only to tell an unknown `-W` flag from one we do not implement; a different set, not a copy of the registry) and the option parsing in `diag.c`, which need text. `gcc_wopts.h` is left alone.

Expected gain: at most the 64M of `find_index_cached` (1.65%); the memo and level logic in `diag_level_for` stay. The change is justified as a simplification (one table, a compile-time check, a cache and a search deleted); the instruction count is checked, not promised.

## Staging

One commit for the conversion, because the entry points and ~630 sites must agree. Steps:

1. Add `diag_opts.def` by script from the current table (a one-time script, not kept), `DiagOpt`, `diag_opt_find`; switch `options[]` to the def. Build still passes: nothing calls the new things yet.
2. Change the entry-point and wrapper signatures. A script rewrites each string literal that is the id argument of a listed call (matched by function name and argument position, not by text: `"undef"` also means a token kind and `"restrict"` a keyword elsewhere) to its `DW_` identifier; the script fails on a literal that is in id position and not in the table. The build then lists what is left (ternaries, data tables, wrappers), which is fixed by hand.
3. Delete the cache and the old lookups.

## Checks

- `diag_selftest` gains: for every row, `diag_opt_find(options[i].name) == i + 1`, names are distinct, and `DW_NONE` is `""`.
- Output must not move: the goldens in `tests/run.sh` (2347 at this point), `verify.sh` counts, `san` (2346, findings 0) and the globals check, all unchanged.
- The conversion script's output is diffed: the number of rewritten sites is printed and compared with the 630 count, and no site is rewritten twice.
- callgrind on `uvloop/loop.c` before and after (deterministic; same flags as the gate). Expect the drop to be at most 64M; any rise is investigated before the commit.

## Risks

- Width: about 35 files change in one commit (34 files outside `diag.c` hold option-name literals, counted with the common non-option words excluded). The conversion is mechanical, and the sites it cannot rewrite are exactly the ones the compiler rejects.
- A call that passes an id the registry does not contain (today treated as "no option") either gets added to the table or is found as a bug. The script lists them before step 2 completes; if there are any, they are reported to the user, not settled silently.
- Other sessions editing diag call sites in parallel conflict with the rewrite. The conversion runs against a clean `main` and is rebased by re-running the script, not by hand.

## Alternatives considered

- **Runtime interner for ids** (the interner of `intern.h`): rejected. The set is closed and known at compile time, so a runtime table still costs a lookup per call.
- **Keep strings, delete the cache only** (h2): no signature change and no compile-time check, and it gives up most of the 1.65%.
- **A generator script and a generated header** instead of the X-macro: adds a build step and a generated file to keep fresh; the X-macro needs neither.
- **A typed pointer to the table row** (`const DiagOption *`) instead of an enum: also strict, but the pointer cannot index the `memo`, `overrides[]` and `optlevel[]` arrays without a subtraction, and enum values are stable across the process.

## Outcome (2026-11-01, `237b3a2`, `723f951`)

- The table is `src/diag_opts.def` (187 options), the enum is `DiagOpt`, and the row for `DW_X` is `options[DW_X - 1]`. `diag_selftest` checks every row's enum and the `DW_NONE` case.
- 792 string literals in id position were rewritten by script (the design's 630 counted files, not calls), 12 ternary arguments by a second pass, the rest by hand (wrappers, `Lit.id`, `ConvInfo.warnopt`, `opt` variables in `cconv.c`/`cdecl.c`/`cformat.c`). 39 files changed, +973/-960.
- Unregistered ids (the Risks case): four, `invalid-memory-model`, `extra`, `c2x`, `inputloc`. The user chose tags ("n1"): `DIAG_TAG` rows in the `.def`, enum values above `DW_COUNT`, kept out of `options[]` so no flag or listing code skips them. Making them options is parked (`PARKED.md`).
- Deleted: `find_index_cached`, `DiagEngine.idc_key`/`idc_val`, the per-call name search. Kept: `by_name`, `find_index_scan`, the `memo`. `-Werror=int-conversion` is in `WARN`.
- Gate on `723f951`: run 2347/0, san 2346/0 (findings 0), verify 637/638, 289/289, 3908/3910, globals 36. callgrind `uvloop/loop.c`: 3,896,168,683 -> 3,819,218,212 (-76.95M, -1.97%), above the 64M ceiling the design expected; the extra is probably the per-call string checks that went with the lookup (inference, not profiled).
