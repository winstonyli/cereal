---
status: active
---

# Diagnostic option ids as a generated enum: implementation plan

> **For agentic workers:** execute inline, task by task; steps use `- [ ]`.

**Goal:** the diagnostic entry points take a `DiagOpt` enum generated from one table (`src/diag_opts.def`) instead of an option-name string.

**Architecture:** the registry rows move to a `.def` file included twice (the enum in `diag.h`, the rows in `diag.c`); the enum value is the row index. Strings stay at the boundary (`Diagnostic.id`, `-W` parsing, pragmas, `--only=`, cells) through `diag_opt_find`.

**Spec:** `docs/DIAG_IDS_DESIGN.md` (approved 2026-11-01, "approve").

## Global constraints

- Output must not move: `tests/run.sh` (2347 passed, 0 failed at `02bfab6`), `verify.sh` counts (637/638, 289/289, 3908/3910), `san` (2346/0, findings 0), `globals.sh` (36 allowed) all unchanged.
- No stored format moves: `Diagnostic.id` stays `const char *`; no cell version bump.
- No compatibility shim: no overload that still takes a string; the conversion is one commit.
- Files are LF. Edit with the Edit tool or the scripts below (explicit UTF-8, `newline=''`); `git diff --stat` before each commit.
- Gate cadence: the quick loop after each task; the full gate (`sh bench/tools/gate.sh` in WSL, started detached with `setsid nohup`, about 10 minutes) once before the T2 commit.
- Python never reads stdin: run scripts as `python3 -P file.py < /dev/null`.

## Review focus

- A call whose id is a string the registry does not contain (found 2026-11-01: `"invalid-memory-model"`, `"extra"`, `"c2x"`, `"inputloc"`). See **Decision**: they are tags.
- Ternaries and `id ? id : ""` forms: the script leaves them for hand (16 found); each must keep its meaning, with `""` becoming `DW_NONE`.
- The string boundary: `diag_map_enabled(map, const char *id)` and `fixmode.c` still receive `Diagnostic.id` strings.
- A misspelled name used to be silent; after T2 the compiler checks every site. The script's `unknown` list is the same check done early.

## Decision: unregistered ids (answered 2026-11-01, "n1 n3")

Four names were used as ids without being in the registry: `invalid-memory-model` (`ccall.c`), `extra` (`cwarn_expr.c`), `c2x` and `inputloc` (`lit.c`, read back by `cexpr.c`). They became **tags**: `DIAG_TAG(ID, "name")` rows in `diag_opts.def`, enum values above `DW_COUNT`, named by `tag_names[]` in `diag.c`, kept out of `options[]`. So no flag matches them, `--list-warnings` omits them and `diag_level_for` returns the requested level (`opt_index` is -1), as before; no skip logic in `name_matches` or the listing. Making them real options is parked (`PARKED.md`, "Unregistered diagnostic ids").

## File structure

- Create `src/diag_opts.def` (the 187 rows, plus the tag rows) by T1's script.
- Modify `src/diag.h` (enum, new signatures, `DiagEngine` loses `idc_key`/`idc_val`), `src/diag.c` (rows from the `.def`, `diag_opt_find`, no id cache), about 35 files under `src/` for the call sites, `Makefile` (`-Werror=int-conversion`), `docs/CLEANUP_PLAN.md`, `docs/DIAG_IDS_DESIGN.md`.

### Task 1: the table, the enum, `diag_opt_find` (additive)

**Files:** create `src/diag_opts.def`; modify `src/diag.c`, `src/diag.h`.

**Interfaces:** produces `enum DiagOpt` (`DW_NONE = 0`, `DW_<ID>`..., `DW_COUNT`), `DiagOpt diag_opt_find(const char *name)`, `const char *diag_opt_name(DiagOpt)`.

- [ ] **Step 1:** write `gen_def.py` (below) to the scratchpad and run it from the repo root. Expected output `rows 187`; `src/diag.c` now includes the `.def` inside `options[]`.
- [ ] **Step 2:** in `src/diag.h` add, before `diag_enabled`:

```c
typedef enum DiagOpt {
    DW_NONE = 0,            /* a hard error, or anything without an option: id "" */
#define DIAG_OPT(id, ...) DW_##id,
#include "diag_opts.def"
#undef DIAG_OPT
    DW_COUNT
} DiagOpt;

/* The option called name (text from a flag, a pragma, a stored diagnostic); DW_NONE if none. */
DiagOpt diag_opt_find(const char *name);
const char *diag_opt_name(DiagOpt o);   /* "" for DW_NONE */
```

- [ ] **Step 3:** in `src/diag.c`, after `NOPTIONS`, add `_Static_assert(NOPTIONS == DW_COUNT - 1, "diag_opts.def and options[] disagree");` and, after `find_index`:

```c
DiagOpt diag_opt_find(const char *name)
{
    long i = find_index(name);
    return i < 0 ? DW_NONE : (DiagOpt)(i + 1);
}

const char *diag_opt_name(DiagOpt o)
{
    return o == DW_NONE ? "" : options[o - 1].name;
}
```

- [ ] **Step 4:** in `diag_selftest` (`src/diag.c`) add: for every `i < NOPTIONS`, `diag_opt_find(options[i].name) == i + 1` and `!strcmp(diag_opt_name(i + 1), options[i].name)`; also `diag_opt_find("no-such-option") == DW_NONE` and `diag_opt_name(DW_NONE)[0] == 0`. Count failures into the existing `bad` counter.
- [ ] **Step 5:** build and run the quick loop: `make -j4 2>&1 | grep -E "error|warning"`, then `GOLDEN_JOBS=6 PATH=$HOME/tools/z3:$PATH EVAL_SWEEP_SOLVE="z3 -in -smt2" sh tests/run.sh 2>&1 | tail -3` in `~/cereal-s1` after `sh .sync.sh`. Expected `2347 passed, 0 failed` (the selftest checks live inside the existing `diag` selftest entry) and no compiler output.
- [ ] **Step 6:** `git diff --stat`; commit `diag: option table in diag_opts.def, DiagOpt enum, diag_opt_find`.

### Task 2: convert the entry points and every call site (one commit)

**Files:** `src/diag.h`, `src/diag.c`, `Makefile`, and every file the script and the compiler touch.

**Interfaces:** consumes T1. Produces: `diag_report`, `diag_vreport`, `diag_enabled`, `diag_option_state`, `diag_option_explicit`, `diag_option_requested`, `diag_option_level`, `diag_option_size` take `DiagOpt` where they took `const char *id`; `Diagnostic.id` stays `const char *`; `diag_map_enabled(const uint8_t *map, const char *id)` is unchanged.

- [x] **Step 1 (decision):** tags as in **Decision** above (done as `DIAG_TAG` rows outside `options[]`, not as group-`"tag"` rows that every consumer would have to skip).
- [ ] **Step 2 (dry run):** write `rewrite_ids.py` (below) to the scratchpad and run it without `--write`. Expected before the tag rows: `rewritten 786 unknown-literal-in-id-position 6 left-for-hand 16`; after them, unknown is 0 (a name left over is a new row or a bug: report it).
- [ ] **Step 3:** retype the base functions in `diag.h`/`diag.c`: the `const char *id` parameter of `diag_report`, `diag_vreport`, `diag_enabled`, `diag_option_*` becomes `DiagOpt`; in `diag_vreport` set `dg->id = diag_opt_name(opt)`; `diag_level_for` indexes `options[opt - 1]` (`DW_NONE` returns `requested`); delete `find_index_cached`, `idc_key`, `idc_val` and the `_Static_assert` on 512. Add `-Werror=int-conversion` to `WARN` in the `Makefile`.
- [ ] **Step 4:** run `rewrite_ids.py --write`; `git diff --stat` must list only call-site files; its `rewritten` count equals the dry run's.
- [ ] **Step 5:** retype the wrappers the script's table names (`cwarn`, `cwarn_d`, `cpedwarn`, `cpedstd`, `vrep`, `vped`, `iwarn`, `pvreport`, `pwarn_opt`, `pp_warn_at`, `cc90`, `cc90_id`, `conv_diag`, `lit.c` `say`/`note` and `Lit.id`) and the data-table fields that hold an option name (`ci.warnopt`, the `opt` variable in `cconv.c`), then `make -j4 2>&1 | grep -E "error|warning"` and fix what the compiler lists, by hand, one file at a time. The 16 `HAND` lines are among them: `""` becomes `DW_NONE`, `id ? id : ""` becomes `id`, a ternary of literals becomes a ternary of `DW_` identifiers. Repeat until the build is clean.
- [ ] **Step 6:** callers that hold a name as text and need the enum (`#pragma GCC diagnostic` in `cpragma.c`, flag parsing in `driver.c`, anything the compiler flags that has a `const char *`) call `diag_opt_find(name)`. List them in the commit message.
- [ ] **Step 7:** quick loop; expected `2347 passed, 0 failed`.
- [ ] **Step 8:** the full gate (detached). Expected: run 2347/0, san 2346/0 and findings 0, verify unchanged, globals 36 allowed.
- [ ] **Step 9:** `git diff --stat`; commit `diag: option ids are a generated enum; the id cache goes`.

### Task 3: measure and close

- [ ] **Step 1:** callgrind on `uvloop/loop.c` with the gate's flags (`-fsyntax-only -std=c99 -w ...`, see `bench/tools/gate.sh`). The baseline before T2 is 3,896,168,683 (`cb32929`). Expected: lower by at most about 64M; a rise is investigated before closing.
- [ ] **Step 2:** `docs/DIAG_IDS_DESIGN.md`: `status: done` and an Outcome (sites rewritten, hand sites, tag rows or not, the counts, the callgrind change); `docs/CLEANUP_PLAN.md` IN1: a Done line for the diag part; grep `docs/` for `find_index_cached`, `idc_key` and "pointer cache" and fix stale wording. This plan: `status: done`.
- [ ] **Step 3:** commit `IN1: close the diagnostic ids work`.

## Scripts

Both were dry-run on `02bfab6` on 2026-11-01 (`gen_def.py` on a scratch copy, reverted; `rewrite_ids.py` without `--write`).

### `gen_def.py` (T1)

```python
# usage: python3 -P gen_def.py < /dev/null   (from the repo root)
# writes src/diag_opts.def and replaces the rows in src/diag.c
import re
p = 'src/diag.c'
s = open(p, encoding='utf-8', newline='').read()
a = s.index('static const DiagOption options[] = {\n') + len('static const DiagOption options[] = {\n')
b = s.index('\n};', a) + 1
body = s[a:b]
def ident(n):
    return 'DW_' + n.replace('+', 'P').replace('=', '_V').replace('-', '_').upper()
out, rows = [], 0
for line in body.split('\n'):
    m = re.match(r'^(\s*)\{"([^"]*)",(.*)\},$', line)
    if m:
        out.append('DIAG_OPT(%s, "%s",%s)' % (ident(m.group(2))[3:], m.group(2), m.group(3))); rows += 1
    elif line.strip().startswith('{"'):
        raise SystemExit('row spans lines: ' + line)
    elif line.strip().startswith('/*'):
        out.append(line.strip())
    elif line.strip():
        raise SystemExit('unexpected line: ' + line)
assert rows == 187, rows
open('src/diag_opts.def', 'w', encoding='utf-8', newline='').write(
    '/* diag_opts.def - every configurable diagnostic: DIAG_OPT(ID, name, group, level, on, by, help).\n'
    ' * Included by diag.h (the DiagOpt enum) and diag.c (the registry); the row for DW_ID is\n'
    ' * options[DW_ID - 1].  Hard errors use DW_NONE and are not listed. */\n' + '\n'.join(out) + '\n')
s = s[:a] + '#define DIAG_OPT(id, ...) {__VA_ARGS__},\n#include "diag_opts.def"\n#undef DIAG_OPT\n' + s[b:]
open(p, 'w', encoding='utf-8', newline='').write(s)
print('rows', rows)
```

### `rewrite_ids.py` (T2)

```python
# usage: python3 -P rewrite_ids.py [--write]   (run from the repo root; never reads stdin)
import re, glob, sys, collections
BS = chr(92)
WRITE = '--write' in sys.argv
src = open('src/diag.c', encoding='utf-8').read()
a = src.index('static const DiagOption options[]'); b = src.index('\n};', a)
names = re.findall(r'^\s*\{"([^"]*)"', src[a:b], re.M)
def ident(n):
    return 'DW_' + n.replace('+', 'P').replace('=', '_V').replace('-', '_').upper()
ID = {n: ident(n) for n in names}
assert len(set(ID.values())) == len(ID), 'derived identifiers collide'
# callee -> index of its option-id argument; limited to files when the name is generic
CALLS = {'cwarn': 2, 'cwarn_d': 3, 'cpedwarn': 2, 'cpedstd': 3, 'vped': 2, 'vrep': 2, 'iwarn': 3,
         'diag_report': 2, 'diag_vreport': 2, 'diag_enabled': 1, 'diag_option_state': 1,
         'diag_option_explicit': 1, 'diag_option_requested': 1, 'diag_option_level': 1,
         'diag_option_size': 1, 'pp_warn_at': 2, 'cc90': 2, 'cc90_id': 1, 'conv_diag': 2,
         'pwarn_opt': 1, 'pvreport': 2}
ONLY = {'note': ('src/c/lit.c', 2), 'say': ('src/c/lit.c', 2)}
CALLRE = re.compile(r'\b(' + '|'.join(list(CALLS) + list(ONLY)) + r')\s*\(')
def split_args(t, i):
    """t[i] is just after '('; return [(start, end)] of the top-level arguments."""
    out, depth, start, k, n = [], 0, i, i, len(t)
    while k < n:
        c = t[k]
        if c == '"':
            k += 1
            while t[k] != '"':
                k += 2 if t[k] == BS else 1
        elif c == "'":
            k += 1
            while t[k] != "'":
                k += 2 if t[k] == BS else 1
        elif c in '([{': depth += 1
        elif c in ')]}':
            if depth == 0:
                out.append((start, k)); return out
            depth -= 1
        elif c == ',' and depth == 0:
            out.append((start, k)); start = k + 1
        k += 1
    return out
stats = collections.Counter(); unknown = []; left = []
for f in sorted(glob.glob('src/**/*.[ch]', recursive=True)):
    f = f.replace(BS, '/')
    if f in ('src/diag.c', 'src/gcc_wopts.h'): continue
    t = open(f, encoding='utf-8', newline='').read(); edits = []
    for m in CALLRE.finditer(t):
        name = m.group(1)
        if name in ONLY:
            if f != ONLY[name][0]: continue
            idx = ONLY[name][1]
        else:
            idx = CALLS[name]
        args = split_args(t, m.end())
        if idx >= len(args): continue
        s, e = args[idx]; text = t[s:e].strip()
        lit = re.fullmatch(r'"([^"]*)"', text)
        if lit:
            n = lit.group(1)
            if n == '': new = 'DW_NONE'
            elif n in ID: new = ID[n]
            else:
                unknown.append((f, name, n)); continue
            edits.append((s + t[s:e].index(text), s + t[s:e].index(text) + len(text), new)); stats['rewritten'] += 1
        elif '"' in text:
            left.append((f, name, text.replace('\n', ' ')[:70]))
    if edits and WRITE:
        for s, e, new in sorted(edits, reverse=True): t = t[:s] + new + t[e:]
        open(f, 'w', encoding='utf-8', newline='').write(t)
print('rewritten', stats['rewritten'], 'unknown-literal-in-id-position', len(unknown), 'left-for-hand', len(left))
for u in unknown: print('UNKNOWN', u)
for l in left[:60]: print('HAND', l)
```
