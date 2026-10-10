# A4 design: compile-commands replay, response files, `-M*` output

Written 2026-10-09 against `abc8183`. Design only: nothing here is
implemented. ROADMAP track A4. Claims about the code cite `file:line` at
that commit; claims about gcc-13 were probed with gcc 13.4.0 on WSL
(section 10 lists the probes). Numbers marked "est." are estimates.

## 1. Decisions in one place

| # | Decision | Why |
|---|----------|-----|
| D1 | One path mechanism: a per-TU working directory (`SrcMgr.cwd`). Relative paths anywhere (main file, `-I`, `-include`, `#include` from a relative directory) resolve against it; `SrcFile.path` becomes the absolute key, `SrcFile.name` keeps the spelling. | gcc runs each entry in its `directory`; this reproduces its file names in diagnostics and deps without knowing which options carry paths. Replaces per-option path rewriting for compile_commands entries. |
| D2 | One function turns a compile command into `Options` (`entry_options`, new `src/compdb.c`), used by the CLI replay and the language server. | Today the LSP has the only copy (`config.c:241`); the brief forbids a second parser. |
| D3 | Option parsing never calls `fatal()` and never writes to `stderr` directly; problems go to `Options.msg` and count in `bad_options`. | Per-entry option errors must not kill a replay of 5,000 entries, and messages must be attributable. |
| D4 | The default standard becomes `gnu17` (gcc-13's default) everywhere. | Most compile commands carry no `-std`. Today cereal's default is strict C99 (`__STRICT_ANSI__` 1, `__STDC_VERSION__` 199901L, probed), so `strdup` after `<string.h>` gives two false warnings that gcc-13 does not give. Open question Q1. |
| D5 | Replay never writes dependency files; `-M*` in entries are parsed and dropped. | Writing `.d` files into a build tree as a side effect of a check is never wanted. |
| D6 | One argument splitter with two quoting modes: shell (compile_commands `command`, `.cereal` files) and gcc (`@file`, libiberty `buildargv`). | The two syntaxes differ only in what a backslash does inside quotes. |
| D7 | `-M*` output is one PP listener (records dependencies on `#include` events) plus one writer called from `finish()` (`main.c:68`), the exit path every TU mode already shares. | Covers `-E`, `check`, `parse`, `lint`, sequential and parallel, without per-mode code. |
| D8 | No `cereal deps` mode word; `-M`/`-MM` are the interface. | `cereal query deps` already means the macro dependency closure (`main.c:1057`); a third "deps" would confuse. ROADMAP's wording "the include graph and `query deps` exist" is misleading: `query deps` has nothing to do with files. Open question Q3. |
| D9 | Exit status of a replay: 0 clean, 1 some file has errors, 2 usage error, 3 no errors but some entries could not be checked. | CI must not go green silently when cereal skipped work, but must be able to tell "your code has errors" from "cereal cannot read this entry". Open question Q2. |

## 2. What exists (verified at abc8183)

- `cereal check|-fsyntax-only|parse|lint|-E FILE...` run every input
  through `run_inputs` (`main.c:336`): a shared pool of `-j` threads
  (`Options.jobs`, default `cpu_count()`, `thread.c:141`; the calling thread
  helps, so `-j12` means 12 threads), output and diagnostics buffered per TU
  in `open_memstream` and committed in input order. Every TU job shares one
  `Options`.
- `finish()` (`main.c:68`) is the common end of `preprocess_one`,
  `lint_one` and `parse_one`: it prints diagnostics and returns 1 when the
  TU has errors (`diag.nerrors`, which counts `-Werror` promotions).
- Option parsing: `options_parse_one` (`driver.c:253`) and the skip table
  `ignored_opts` (`driver.c:123`, A2; `-c`, `-S`, `-fPIC` are
  `IGN_SILENT`). `-MD -MMD -MP -MG -MF -MT -MQ` are
  `IGN_DEPS`: consumed, noted ("accepted but no dependency file is
  written", `main.c:1231`). `-M` and `-MM` are not in the table: today they
  are "unknown option", exit 2 (probed). Missing option arguments, an
  unsupported `-std=`, bad `-fparallel=`/`-j` call `fatal()` (exit 2,
  `driver.c:107,178,204,282,365,372`); unknown and malformed options print
  to `stderr` directly (17 sites in `driver.c`) and count `bad_options`
  (exit 1, `main.c:1235`).
- The default (no `-std=`) is `std_year` 1999 with `gnu_mode` false
  (`driver.c:17`): C99 with the host's `-std=c99` predefines.
- compile_commands is read only by the language server: `load_db`
  (`config.c:86`, `arguments` preferred over `command`, `command` split by
  `shell_split`, `config.c:14`), `config_options_for` (`config.c:241`)
  skips `argv[0]`, non-option arguments, `-E`, `-o X`, then
  `option_ignored`, then rewrites `-I -iquote -isystem -include` paths to
  absolute against the entry's `directory` (`add_flag`, `config.c:180`).
  `-idirafter`, `-imacros`, `--sysroot` are not rewritten (they are
  `IGN_SEMANTIC` anyway). The LSP sets `Options.lenient`.
- Source files: `SrcFile.path` ("as opened, normalized") and
  `SrcFile.name` ("presumed name for diagnostics/`__FILE__`") exist but are
  always equal (`srcmgr.c:102-103`); every file read goes through
  `load_locked` (`srcmgr.c:177`), which opens the normalized path.
  Quote includes join the includer's `path` directory (`pp.c:1853`). Other
  file-system calls on paths: `stat` in `ppexpand.c:1053,1134` (on
  `SrcFile.path`) and `par.c:1032` (on the input path as given).
- `#include` raises `PP_EMIT(include)` with an `IncludeEvent` (spelled
  name, angled, from, result, `pp.h:130`) and `file_enter` on success
  (`pp.c:2046-2088`). The include frame knows `dir_index` and `system`
  ("as of this point", `pp.h:202,209`). The parallel preprocessor's phase A
  runs on the TU's own `PP` after `tu_begin` (`par.c:1097-1101`,
  `pp_run_phase_a` drives `pp_next`), so a listener on `tu->pp` should see
  every include once and in order (confirmed by an instrumented run,
  probe 2026-10-10, 6.10).
- Each TU owns its `SrcMgr` (`driver.h:54`, initialised in
  `tu_init_shared`, `driver.c:652`); no file cache is shared between TUs.
- The `<command line>` virtual file is marked `system_header`
  (`pp.c:882`).
- `host_predefs` come from `gcc -std=c99 -dM -E /dev/null`
  (`tools/probe-host.sh`), so the macros of `/usr/include/stdc-predef.h`
  are baked in and that file is never opened.
- Thread-safety gaps for parsing options on worker threads: `cfg_gen_next`
  (`diag.c:234`) is a plain counter; `pp_options_finish` calls `localtime`
  (`pp.c:710`).

## 3. Shared foundations

### 3.1 Per-TU working directory (D1)
`Options.cwd` (absolute, NULL = the process's) is copied to `SrcMgr.cwd`
in `tu_init_shared`. In `load_locked`:
- key/`path` = `path_normalize(cwd + "/" + p)` when `p` is relative and
  `cwd` is set, else `path_normalize(p)` as today;
- `name` = `path_normalize(p)` (the spelling, relative when given relative).

Quote includes join `path_dirname(includer->name)` instead of `->path`
(`pp.c:1853`); with `cwd` NULL the two are equal, so nothing changes for
existing runs. `par.c:1032` stats the main file through the source manager
(or `cwd + path`). `ppexpand.c` already stats `SrcFile.path`, now absolute.

Effect in a replay: diagnostics, `__FILE__` and `-E` line markers show
`../src/a.c` exactly when gcc run in the entry's `directory` would, while
caching, overlays and `stat` use the absolute path. Relative `-I`,
`-iquote`, `-isystem`, `-include` and `#include` all resolve against the
entry's directory with no per-option code.

The language server adopts it too (via D2). Arguments that do not come
from the entry (cereal's own options in a replay, `.cereal` files in the
LSP) have a different base directory than the entry, which one `cwd`
cannot express; for those the existing `path_opts`/`add_flag` rewrite
(`config.c:176-201`, `-I -iquote -isystem -include` to absolute) is kept
as the single place that knows which options carry paths, and both users
call it (3.5). The LSP shows `SrcFile.name` in four places
(`features.c:334,931,1617`, `index.c:1724`); they switch to `path` so
hover and hint texts stay absolute (`tests/lsp/proj.expected` must not
change). The other `name` users in `index.c` (2151, 2345, 2513) belong to
`index`/`query`, which replay rejects (4.1), so they never see a `cwd`.

Not covered: gcc keeps unnormalized spellings (`./a.c`, `sub/../x.h`)
where cereal normalizes `name` lexically (existing behaviour, unchanged).
The deps writer (6.3) computes gcc's spelling itself and is not affected.

### 3.2 Option messages and errors (D3)
- `Options.msg` (FILE *, default `stderr`, NULL = silent); one helper
  `opt_error(o, fmt, ...)` prints `cereal: error: ...` there and increments
  `bad_options`. The 17 `fprintf(stderr, ...)` in `driver.c` and the six
  `fatal()` calls in option parsing use it; a missing argument consumes
  what is there and reports.
- CLI effect: those six cases exit 1 instead of 2, as gcc's driver does
  for bad command lines. Goldens or scripts that test exit 2 for them are
  updated (grep before the change).
- The `ignored_semantic` notes stay a vector; replay aggregates them (4.8).

### 3.3 Default standard (D4)
`options_init` calls `set_std(o, "gnu17")`. Most checks pass `-std`
explicitly (`tests/run.sh:329,404,444,461` for cereal; `par.py` adds it;
`gate.sh:14`), but not all: run.sh section 1's `diff_pp` passes
`-std=c99` only to `$REFCC` (`run.sh:42`) and runs `cereal -E` with the
default (`run.sh:43`), so the same commit adds `-std=c99` there. LSP
sessions, query scripts and lint goldens run without `-std` and may move;
every changed golden is reviewed against gcc-13's `gnu17` behaviour
before it is accepted.

### 3.4 Argument splitting (D6)
`split_args(Arena *, const char *, SplitMode)` replaces `shell_split`
(moved from `config.c:14` to `compdb.c`):
- `SPLIT_SHELL` (as today): `'...'` literal; `"..."` with `\` escaping only
  `" \ $ ` newline`; `\` escapes any character outside quotes.
- `SPLIT_GCC` (libiberty `buildargv`, probed): `\` escapes the next
  character everywhere, including inside `'...'` (`'x\y'` gives `xy`,
  `'it\'s'` gives `it's`); quotes group and are removed; `''` gives an
  empty argument; `#` is not special.

### 3.5 Compile command to Options (D2)
`compdb.c` holds `CompileEntry {dir, file (as written), abs_file, argc,
argv}`, `compdb_load(Arena *, path, VEC *)` (today's `load_db` plus: a
relative `directory` resolves against the database file's directory; an
entry without `file` or without both `arguments` and `command` is
reported and skipped) and

```
/* Options for one entry: its arguments, then `extra` (already made
 * absolute with add_flag; later wins), then options_finish.  Never
 * writes dependency output. */
void entry_options(Options *o, const CompileEntry *e,
                   const char *const *extra, int nextra);
```

which sets `o->cwd = e->dir`, expands `@file` arguments relative to it
(slice 2), walks `argv[1..]` as `config.c:250-266` does today (skip
non-option arguments: the entry's source and wrapper words such as the
`gcc` after `ccache`; skip `-E`, `-fsyntax-only`, `-o X`; `option_ignored`;
`options_parse_one`) but without rewriting paths (D1 does that), then the
extras, clears the dependency request (D5), calls `options_finish`, and
computes `o->fingerprint` over `cwd` and every argument except `-o X`,
arguments starting with `-M` (with the values of `-MF -MT -MQ`), `-c`,
`-S` and `IGN_SILENT` ones (none of them changes diagnostics). `add_flag` moves to `compdb.c` with it. The two callers:
- replay: extras are cereal's own options, made absolute against the
  process directory;
- `config_options_for`: sets `lenient` first (unknown flags ignored, as
  today) and passes the `.cereal` flags as extras, made absolute against
  each `.cereal` file's directory as today (`config.c:203-239`).

### 3.6 Jobs with their own options
`TuJob` gains `const CompileEntry *entry` (NULL for plain inputs).
`run_tu_job` builds a local `Options` with `entry_options` (messages
silenced: phase 1 already reported them, 4.8), runs the mode's `TuFn`, and
frees it, so peak memory is one `Options` per running job (a `DiagConfig`
is about 10 KB est.; 5,000 kept at once would be ~50 MB for nothing).
`run_inputs` becomes a thin builder over `run_jobs(run_opts, jobs, n, fn,
out)`, which returns counts per status instead of OR-ing exit codes.
Entry options inherit the run's `__DATE__`/`__TIME__` strings (one
timestamp per run, as one build) instead of calling `localtime` on worker
threads; `cfg_gen_next` becomes atomic.

## 4. Part 1: `--compile-commands DB [FILES]`

### 4.1 Shape
```
cereal check --compile-commands DB [cereal options] [FILES]
cereal check --compile-commands=DB ...
```
Allowed with every `run_inputs` mode (`check`, `-fsyntax-only`, `parse`,
`lint`, `-E`): the job list comes from the database instead of the inputs,
the mode is unchanged. `index`, `query`, `lsp` with it, a missing or
unreadable DB, a DB that is not a JSON array or has no entries, a FILE
that matches no entry, and `-o` among cereal's own options are usage
errors (exit 2, nothing runs).

### 4.2 Entries
- `arguments` preferred over `command` (the JSON Compilation Database
  spec; `command` is split with `SPLIT_SHELL`).
- `directory` is the entry's `cwd` (D1); `file` is opened relative to it
  and shown as written.
- `output` is ignored.
- Without FILES: every entry in database order. With FILES: each FILE is
  made absolute against the process directory and normalized; an entry is
  selected when its `abs_file` equals it or, when FILE is a directory, lies
  under it. Database order is kept.

### 4.3 Language
gcc's rule: the last `-x LANG` before the source decides, else the suffix
of `file`, read on the arguments after `@file` expansion (Round 207: the
list is expanded once, by `entry_expand`, and both `entry_is_c` and
`entry_options` read that list, so a `-x` inside a response file counts).
Only C (`.c`, or `-x c`) is checked; anything else (`.cc .cpp
.cxx .C .m .S .s .i .h`, `-x c++`, `-x c-header`) is skipped and counted
as "not C", which does not affect the exit status. `.h` is gcc's
`c-header`, which A2 rejects; checking headers is Q8. `argv[0]` is not
consulted, so `g++ x.c` (gcc compiles it as C++) is checked as C: a known
gap, documented.

### 4.4 Options and their order
Entry arguments first, then cereal's own options from its command line
(minus `--compile-commands`, the mode word and FILES), so `cereal check
--compile-commands DB -Wall -Werror -std=gnu11` adds warnings, makes them
errors and overrides the standard for every entry. Relative paths in the
entry resolve against its directory (D1); cereal's own relative `-I
-iquote -isystem -include` resolve against the process directory, through
the `add_flag` rewrite (3.1, 3.5).

### 4.5 Dedupe
Two selected entries with the same `abs_file` and the same fingerprint
(3.5) are checked once. CMake's duplicate entries for a file built into a
static and a shared library differ only in `-o`, `-MF` and `-fPIC`, so
they collapse; entries that differ in `-D`, `-I` or `-std` are both
checked (their diagnostics appear once per entry, in database order).

### 4.6 Running
- `-j N` is cereal's own (an entry's `-j` would be parsed and ignored);
  default `cpu_count()`, unchanged. On this 16-core machine scripts, tests
  and gates pass `-j12` and run under `nice` (STATUS working conventions);
  in-TU parallel work shares the same pool, so the cap holds.
- Phase 1 (main thread, sequential, cheap): select, classify language,
  parse each entry's options once to collect messages, the fingerprint
  and the status (option errors, unsupported `-std`, missing file), and
  free them. Phase 2: `run_jobs` over the entries left.
- Output is committed in database order, byte-identical for any `-j`
  (the existing `run_inputs` guarantee, extended to replay in run.sh
  section 7).
- A `fatal()` inside a TU (out of memory, too many files) still ends the
  whole run with exit 2; only option parsing was made non-fatal.

### 4.7 Output
Diagnostics are the TU's own, unchanged: gcc's text with file names as
spelled relative to the entry's directory (D1). When an entry's directory
differs from the previous entry's (the first: from the process
directory), `cereal: Entering directory '/abs/dir'` is printed to stderr
before its output; it is decided when the job's buffers are committed (in
database order), so the lines are the same for any `-j`. This is the
convention of make that Emacs' compilation mode
and other tools use to resolve relative names. CMake databases (one build
directory, absolute paths) print it once. Under
`-fdiagnostics-format=json` each TU prints one JSON array on stdout as
today (concatenated, like gcc); the directory lines stay on stderr; JSON
consumers lose the directory of relative names (SARIF, A5, can carry a
base URI).

### 4.8 Option problems
Phase 1 captures each entry's messages (`Options.msg` = memstream) and
prints each distinct line once, before the run, with a count and the
first entry: `cereal: note: '-fno-common' is ignored and may change
diagnostics (412 entries, first src/a.c)`, `cereal: error: unrecognized
command-line option '-Qunused-arguments' (3 entries, first lib/b.c)`.
The A2 policy is unchanged: unknown options are errors, semantic ones are
notes, silent ones are silent.

### 4.9 Status, summary, exit
Per selected entry one status: checked clean, checked with errors (the
TU's `finish()` returned 1, including `-Werror`), not checked (option
error, unsupported `-std`, file missing), not C, duplicate. Warnings
alone never fail (gcc semantics); the build's own `-Werror` in an entry
or cereal's `-Werror` decide. One summary line on stderr at the end:

```
cereal: 118 entries: 112 checked (3 with errors), 4 not C, 1 duplicate, 1 not checked
```

Exit (D9): 1 if any checked file has errors, else 3 if any entry was not
checked, else 0; 2 for usage errors (4.1, and bad options among cereal's
own arguments, which outside replay keep exit 1). No progress line: output
already streams in order; a `--progress` is parked until a database is
large enough to want it.

### 4.10 What replay cannot cover
No objects or links (A's scope). Entries from compilers whose flags gcc
does not know (clang-only flags, MSVC `/D` syntax: non-option words are
skipped as inputs, so an MSVC entry would be checked without its flags;
cereal is Linux-only and does not special-case it). Headers without
entries (the LSP's nearest-entry guess, `config.c:159`, is not used;
parked). `g++`-compiled `.c` files. `-imacros`, `-idirafter`, `--sysroot`,
`-ffreestanding` and the other `IGN_SEMANTIC` flags stay unimplemented
(noted once per run). `./`-style spellings show normalized (3.1).

## 5. Part 2: response files `@file`

- Where: once over `main`'s `argv` before anything else (so a response
  file may hold the mode word), and in `entry_options` for an entry's
  arguments, relative to the entry's directory.
- Rules (gcc-13, probed): an argument that starts with `@` is replaced by
  the file's contents split with `SPLIT_GCC`; an empty or whitespace-only
  file gives no arguments; inserted arguments are scanned again, so nested
  `@file` works, each relative to the current directory, not to the file
  that names it (probed: `@n2.rsp` inside `r/n1.rsp` opened `./n2.rsp`);
  at most 2000 expansions, then `too many @-files encountered`;
  `@dir` is `@-file refers to a directory`; `x@y` is not expanded.
- Missing or unreadable file: gcc keeps the argument literally, which later
  fails as an input file. cereal reports `cannot read response file 'F'`
  (exit 1 on the command line; "not checked" in a replay) because in an
  entry a literal non-option word is skipped as an input and the flags
  would vanish silently. A deliberate difference (Q4).

## 6. Part 3: the `-M` family

### 6.1 gcc-13 behaviour (probed)
- `-M`: rule with all headers, implies `-E` with no preprocessed output
  and `-w` (a `#warning` exits 0 and prints nothing). `-MM`: without
  system headers. With `-fsyntax-only` too, only the rule is produced.
  `#error` is still reported and exits 1, and the rule is still printed.
  A missing header is fatal and nothing is printed.
- `-MD`/`-MMD`: the same rule as a side output of the normal run; the
  `.d` file is written although the TU has errors, and not written after
  a fatal missing include. Style precedence (probe 2026-10-10, 20
  combinations of 2 to 4 of the four options, both orders of each pair): the command
  line order does not matter; the strongest present wins in the order
  `-MD` < `-MMD` < `-M` < `-MM` (the order the driver hands them to cc1,
  last wins). So `-MD -MMD` and `-MMD -MD` both give the user-headers-only
  rule, `-M -MMD` and `-MMD -M` the full rule, `-M -MM` and `-MM -M` the
  `-MM` rule. With `-M`/`-MM` and `-MD`/`-MMD` together the rule goes to
  the `-MD` destination (`a-c.d` in link mode, else per 6.1 naming) and
  nothing to stdout.
- Destination: last `-MF` (`-` is stdout), else the `-MD` file name, else
  (for `-M`/`-MM`) `-o`, else stdout. `-MD` file name: from `-o` with its
  last suffix replaced by `.d` (`obj/x.o` gives `obj/x.d`, `obj/noext`
  gives `obj/noext.d`); without `-o`, with `-c`/`-S`/`-E`, the input's
  base name with `.d` in the current directory; without `-o` and without
  those (link mode, which includes `-fsyntax-only`), gcc names it
  `a-BASE.d` (`sub/b.c` gives `a-b.d`, two inputs `a.c c2.c` give `a-a.d`
  `a-c2.d`), except a single input named `a.*` gives `a.d`.
- Targets: `-MT` verbatim, `-MQ` quoted; without either, `-MD`/`-MMD` with
  `-o` and without `-E` use `-o` (quoted), else the input's base name with
  `.o` (quoted; `sub/../a.c` gives `a.o`). `-E -MD -o obj/a.i` gives target
  `a.o` and file `obj/a.d`. `-MT ''` gives an empty target. Order: `-MT`
  targets in order, then the `-MQ` ones rotated left by (number of `-MT`
  mod number of `-MQ`): with no `-MT` they stay in order (1 to 8 probed);
  with one `-MT`, `t1 q2 q3 q1`; with two, `t1 t2 q3 q1 q2`; with five
  `-MT` and three `-MQ`, `q3 q1 q2` (rule fitted to and checked on more than 20
  combinations of 1 to 7 of each, any interleaving, `-M` and `-MD`;
  probe 2026-10-10; the cause in gcc was not found). See 6.9.
- Prerequisites: the main file first, as written minus leading `./`; then
  the `-imacros` files (probe 2026-10-10: they come before the preinclude;
  this line said otherwise); then under `-M` the implicit preinclude
  `/usr/include/stdc-predef.h` (absent with `-nostdinc` or
  `-ffreestanding`, present with `-undef`); then the `-include` files in
  order (relative order of `-include` and `-imacros` on the command line
  is irrelevant: `-include i1 -imacros m1` and the reverse both give
  `a.c m1 stdc-predef i1`); then headers in order of first entry.
  A header's name is the directory it was found through joined with the
  spelled name, unnormalized: `sub/../inc/x.h`, `sub/../sub/y.h` for a
  main file `sub/../a.c`, `s/z.h` for `-Is/` (one trailing slash dropped;
  `-Is//` gives `s//z.h`), `/abs/...` for absolute `-I`. One entry per
  (directory, name) lookup the first time it is entered: the same spelling
  twice is listed once; `inc/z.h` and `./inc/z.h` are listed twice (as
  `inc/z.h` both times); `sub/y.h` reached as `"sub/y.h"` and as `y.h`
  through `-Isub` is listed twice. A guarded header reached under a new
  spelling is listed again (gcc enters it and the guard skips its body); a
  `#pragma once` header is not. `__has_include` adds nothing; `#line`
  changes nothing.
- `-MM` omits a header when it was found in a system directory or its
  includer was in a system header at the `#include` (a header with
  `#pragma GCC system_header` is itself listed, what it includes is not;
  a quote include from an `-isystem` header is omitted). An absolute
  `#include "/usr/include/stdint.h"` from a user file is listed.
- `-MG` (only with `-M`/`-MM`, else `'-MG' may only be used with '-M' or
  '-MM'`): a missing header is listed as spelled (`gen/conf.h`), silently,
  and preprocessing continues; under `-MM` a missing `<...>` header is
  omitted (treated as system).
- `-MP`: a `NAME:` line for every prerequisite but the first.
- `-MF`, `-MP`, `-MT`, `-MQ` without `-M -MM -MD -MMD`: `to generate
  dependencies you must specify either '-M' or '-MM'`.
- Quoting of quoted targets and prerequisites: `$` to `$$`, `#` to `\#`,
  space and tab get a `\` after doubling the backslashes before them
  (`a\ b` gives `a\\\ b`); `:` and other backslashes are left alone.
- Line wrapping: before a name, when the column plus the name's length
  exceeds 72, ` \`, newline and one space; probed at the boundary (column
  69 plus a 3-character name stays, column 70 wraps).

### 6.2 Options
`Options.deps {style (0, 1 user, 2 system), only (from -M/-MM), file,
targets (text, quoted), phony, missing_ok, md_seen}`, parsed by
`options_parse_one`; the `IGN_DEPS` rows, `ignored_deps` and its note
(`main.c:1231`) are deleted. `-c` and `-S` move from `IGN_SILENT` to a
recorded `Options.stage` ('c', 'S', or 'E' from the mode), which only the
`.d` naming reads. Validation (the two gcc errors above) runs in
`options_finish`. `entry_options` clears `deps` (D5).

### 6.3 Mechanism (D7)
- `tu_begin` attaches a `DepsListener` (state in the TU arena, pointer in
  `TU`) when `o->deps.style`; every TU path calls `tu_begin`, including
  the parallel preprocessor's phase A and the sequential rerun after a
  divergence (a fresh TU, a fresh listener).
- `IncludeEvent` gains `int dir_index` (from `pp_find_include`). The
  listener keeps a stack of spelled paths (pushed on `file_enter`, popped
  on `file_exit`) and on each `include` event with result OK or
  SKIPPED_GUARD builds the key (search directory string or the includer's
  spelled directory, plus the spelled name) and records a new key with its
  spelled path and its system flag. SKIPPED_ONCE adds nothing.
  NOT_FOUND under `-MG` records the spelled name.
- System flag (gcc's `MAX(buffer->sysp, dir->sysp)`): found in a system
  directory (`dir_index >= first_system`), or the includer frame's
  `system` is set and the includer is a real file. The `<command line>`
  file is marked system in cereal for diagnostics (`pp.c:882`); gcc's
  command-line buffer is not, so `-include` files count as user files.
  For `-MG` misses: angled, or the includer is system.
- `-MG` sets `PPOptions.missing_ok`: `do_include` (`pp.c:2048`) emits the
  NOT_FOUND event and returns without a diagnostic.
- The writer runs from `finish()` (only of the TU whose result is used, not
  of a phase A TU abandoned for a rerun, 6.10): unless the TU halted on a fatal
  diagnostic, it writes the rule (targets, quoting, wrapping, `-MP`) to the
  destination: the job's `out` stream for stdout (so `-j` keeps order),
  else the file. `-MF FILE` with more than one input is a usage error
  (gcc would overwrite the file; never wanted).
- `-M`/`-MM` select a deps-only mode whatever the mode word (`-E`, `check`,
  `parse`, `lint`), as gcc's `-M` implies `-E`: a TU function that runs
  `tu_begin` + `tu_drain` with `no_warnings` set; `index`/`query` with
  them are usage errors. `-MD`/`-MMD` leave the mode alone.
- The host's preinclude path comes from `tools/probe-host.sh`
  (`gcc -M -x c /dev/null`, second prerequisite) as `host_preinclude`;
  listed under style 2 unless `-nostdinc` or `-ffreestanding` was given.
- The existing include analysis (`analysis/include.c`) is not reused: it
  belongs to `lint`, keys files by `SrcFile` (one per normalized path) and
  keeps no spellings; the deps listener is about 90 lines.

### 6.4 Interaction summary
| Command | Preprocessed/checked output | Deps |
|---|---|---|
| `cereal -M a.c`, `cereal check -MM a.c`, `cereal -E -M a.c` | none | stdout, or `-MF`, or `-o` |
| `cereal -E -MD a.c -o obj/a.i` | `obj/a.i` | `obj/a.d`, target `a.o` |
| `cereal check -c -MMD -MF x.d -o x.o a.c` | diagnostics | `x.d`, target `x.o` |
| `cereal check -MD a.c` | diagnostics | `a.d` (link mode, base `a`) |
| replay entry with `-MD -MF ...` | diagnostics | none (D5) |
| `lsp` | n/a | none (D5) |

### 6.5 Verification against gcc-13
1. `tests/deps/`: small trees with one `*.cmd` per case (the `tests/query`
   runner pattern, generalised to iterate several directories), covering
   every bullet of 6.1. Each case's `.expected` is gcc-13's output, and
   run.sh section 15 also runs the same commands through `$REFCC` and
   requires byte-identical output and exit status.
2. Parallel: section 5 gains `-E -fparallel=on -MD` against `=off`: the
   `.d` files must be byte-identical (section 2's phase A claim was
   confirmed by an event-stream diff, 6.10; this is its regression test).
3. `bench/tools/deps.sh DIR|DB`: for each C file of a corpus (libuv from
   uvloop, Lua from lupa, zstd from zstandard under `~/corpus`, cereal's
   own `src/`) runs `-M`, `-MM`, `-MD -MP -MF` through gcc-13 and cereal in
   a scratch directory and reports "N files, B byte-identical, S same after
   lexical path normalisation"; per-file diffs to `$DEPS_OUT`.
4. The ten `gcc.dg/cpp` tests with `-M`/`-MD`/`-MMD` in `dg-options`:
   the four `missing-{header,sysheader}-{MD,MMD}.c` are `dg-do compile`
   tests expecting the missing-header and "terminated" messages, which
   `par.py` can run once it accepts `-MD`/`-MMD` (in a scratch directory,
   since a run could write `.d` files); report the old and new "files run"
   counts together, as A3 did. `cmdlne-M.c` (`-M` silences `#warning`)
   and `cmdlne-M-2.c` (target `cmdlne-M-2.o:` written to the `-o` file)
   are `dg-do preprocess` tests `par.py` does not model; both become
   `tests/deps` cases. The four `cmdlne-d?-M.c` need `-dD`/`-dI`/`-dM`/
   `-dN`, which cereal does not take; they stay skipped.

### 6.6 Probe results (gcc 13.4.0, 2026-10-10; were: probes still to run)
Run on WSL in throwaway directories, scratch scripts not kept (slice 3
turns them into `tests/deps` cases).
- `-MD -MMD` together: `-MMD` wins in both orders (user headers only); the
  general rule is in 6.1 (`-MD` < `-MMD` < `-M` < `-MM`, command-line order
  irrelevant). Was unverified; the 6.1 sentence "last wins" held only for
  the driver's internal order, not the command line.
- `-MQ` order with 1 to 8 targets: command-line order when there is no
  `-MT`; with `-MT` present the rotation rule of 6.1 (`t1 q2 q3 q1`). The
  earlier "last first" was this rule seen at 2 `-MT` and 3 `-MQ`. `-MT ''`
  with `-MQ q1 -MQ q2` gave `q2 q1` after the empty target.
- `-Wfatal-errors` with `-MD`: the `.d` is not written once a diagnostic
  terminated the TU (`error: expected expression` then "compilation
  terminated due to -Wfatal-errors"; same for a `#error`, even when the
  `#error` is after the includes). Without the option both TUs write the
  `.d` with every header, parse errors or `#error` notwithstanding (exit 1).
  With `-M`: a parse error is not seen (exit 0, rule printed); `#error`
  prints the rule and exit 1, and with `-Wfatal-errors` prints only the
  error (no rule), exit 1. So the writer's "halted" test is "the TU ended by
  a fatal diagnostic", and `-Wfatal-errors` makes every error fatal: needs
  checking that cereal's `-Wfatal-errors` sets `halted` before `finish()`.
- `-M`/`-MD` with a missing `-include FILE` (or `-imacros`): fatal
  `<command-line>: fatal error: FILE: No such file or directory`, exit 1,
  nothing printed, no `.d`. With `-MG` the name is listed as spelled and
  the run continues: an `-include` miss at its place (`a.c stdc-predef.h
  nope.h ...`), an `-imacros` miss before the preinclude (`a.c nope.h
  stdc-predef.h ...`); under `-MM -MG` it is still listed (command-line
  files count as user files). `-include DIR` is the same fatal error.
- `#include "dir"`, `<dir>`, `"dir/"` where `dir` is a directory: fatal
  `dir: No such file or directory` (exit 1, nothing printed, no `.d`, `-E`
  prints the error and the prefix). A directory is skipped in the search:
  `-Ip1 -Ip2` with `p1/x.h` a directory and `p2/x.h` a file lists
  `p2/x.h`. With `-MG` the name is listed as spelled (`dir`), exit 0.
- `#line N "other/dir/name.h"` inside a header before a quote `#include`:
  no effect; the include is searched in the real directory of the file
  (`ln/outer.h` with `#line 50 "fake/dir/name.h"` then `#include
  "inner.h"` listed `ln/inner.h`, also when `fake/dir/inner.h` exists).
  A `#line` in the main file changes nothing either (a quote include from
  the main file still starts at the main file's real directory). This
  confirms "`#line` changes nothing" of 6.1 for headers, and means the
  listener's includer directory must come from the file's path, never the
  presumed name.
- Phase A, see 6.10.

### 6.7 Relation to `cereal deps` and `query deps`
None (D8). If a mode word is wanted later it is an alias that sets `-MM`.

### 6.8 What it cannot cover
- `-imacros` files are not listed until `-imacros` is implemented; when
  they are, they go before the preinclude (6.1).
- `-ffreestanding` is ignored (noted), but it does drop the preinclude from
  the list, as in gcc.
- `SrcFile` is one per normalized path, so cereal's `-E` line markers and
  diagnostics keep the first spelling (3.1); only the deps writer spells
  per lookup.

### 6.9 Known divergences accepted
None from the `-MQ` order any more: the 2026-10-10 probes found the rule
(6.1: `-MQ` targets rotated left by #`-MT` mod #`-MQ`), about three lines
in the writer, so slice 3 copies it and the "accepted divergence" is
dropped. Target order has no meaning to make or ninja, so if the rule
fails on a later gcc version, falling back to command-line order is
harmless.

### 6.10 Parallel preprocessor: one listener, gcc's order (probe 2026-10-10)
Question: does phase A (`par.c:1103`, `pp_run_phase_a` on `tu->pp`) see
every `#include` once and in source order, so that one listener recording
(directory, name) at include time gives gcc's order and de-duplication?
Method: a throwaway copy of the tree (`cp -r`, since deleted) with a
listener attached in `tu_begin` that printed `include` (spelled name,
angled, result, includer path, found path), `file_enter` and `file_exit`
events; each file run with `-E -fparallel=off` and `-fparallel=on
-fparallel-threads=4 -fparallel-chunk=2000` and the two event streams
diffed; the listener also printed `pp->mode` at `tu_begin` (1 = phase A in
the `on` runs, 0 in `off`).
Result: the two streams are identical (all event kinds, including the
skipped-by-guard, skipped-once and not-found results) on uvloop `loop.c`
(632 include events, 433 entries, 8.6 MB), lupa `lua51.c` (444) and
`luajit21.c` (443), zstd `zstd.c` (224), cereal `src/pp.c`, `par.c`,
`main.c`, `lsp/server.c` (203 to 264), and a hand-made file with guards,
`#pragma once`, `#include_next`, macro-computed includes, `__has_include`
and `#if` branches (11 events); a missing include is the same fatal in
both modes (the `not found` event comes before the halt). Each run had one
`tu_begin`. Against gcc-13: the list of distinct files in first-entry
order (from the `file_enter` events, after `realpath -m`) equals gcc-13
`-M`'s list on uvloop (352 headers), lua51, luajit21, zstd and the four
cereal files, after two host differences unrelated to the listener:
`stdc-predef.h` (gcc lists it first as the preinclude; cereal enters it
where a header includes it, so it was removed from both lists) and the
system include directory (cereal reads the host gcc 15's
`gcc/x86_64-linux-gnu/15/include`, gcc-13 its `13`; mapped).
Conclusions and what must be stated in 6.3:
1. Phase A is the full directive pass, so the listener sees what the
   sequential engine sees; no change to the D7 mechanism.
2. The events alone do not give the key: the found file's path is
   normalized (`./inc/guard.h` and `inc/guard.h` arrive as the same
   `SrcFile`), so (directory, name) needs `dir_index` on `IncludeEvent`,
   as 6.3 already says; this probe confirms it is required.
3. With a divergence (`__COUNTER__` under `-fparallel=on`) there are two
   TUs: the phase A one, whose listener saw the whole stream, then a fresh
   sequential one (`mode=0`). The same holds for `PAR_FALLBACK` after
   phase A (by the code, not run). The writer must run only for the TU
   whose result is used (the abandoned TU must never reach its `finish()`
   writer); 6.3 said "a fresh TU, a fresh listener", and this is the
   missing half. `-fparallel=auto` runs a small file with no phase A (one
   TU, mode 0).
4. In phase B the replayed `file_enter` events carry `via = NULL`
   (`pp.c:2928`); a listener that keeps a stack of spelled paths must not
   be attached to a PP that replays a plan. Today only `tu->pp` carries
   the listener, in phase A, which is what the probe saw.
No assumption of section 6 or D7 changes; items 3 and 4 are added
requirements, and 6.5 item 2 (`-E -fparallel=on -MD` vs `=off`) stays as
the regression test.

## 7. Staging, tests, gates, estimates

### Slice 1: replay (DONE, Round 204; first shippable; est. 3 days, realistic 4.5 to 6)
Result: all six steps landed (HISTORY Round 204). Differences from the text
below: the goldens are checked-in `NAME.json` databases with a relative
`directory` (resolved against the database's own directory) and
`NAME.cmd` / `NAME.expected` files (stdout, stderr and exit status, the
checkout path printed as `ROOT`), not `.cmd` files that write their
database; they are run.sh section 16, not section 7; the `-MD -MF` case
checks that no `.d` file appears. `ccdb.sh` wraps `ccdb.py`. Added beyond
the text: the checker's per-check mutable globals became `__thread`
(cattr, cconv, cformat, cinit, cparm, cprint, cwarn_expr; three lazy
caches in diag.c), because ThreadSanitizer showed any `check` of several
files at `-j>1` racing on them, before this slice too.
Steps, each with its check:
1. `compdb.c`: move `split_args` (shell mode), `add_flag`, `CompileEntry`,
   `compdb_load`; `entry_options`; `config.c` calls it. Check: LSP
   sessions unchanged.
2. Option messages (3.2), `cfg_gen_next` atomic, inherited date strings.
   Check: run.sh, with any exit-2 expectations for the six former fatals
   updated.
3. `SrcMgr.cwd` (3.1) and the LSP display sites. Check: run.sh, LSP
   `proj` session byte-identical.
4. Default `gnu17` (3.3) as its own commit. Check: run.sh before and
   after, each changed golden reviewed against gcc-13; `verify.sh`
   numbers unchanged (it passes `-std` explicitly).
5. `run_jobs`, `--compile-commands`, selection, language, dedupe,
   aggregation, summary, exit codes, directory lines, usage text.
6. `bench/tools/mkccdb.sh` (writes a database for a list of files and
   flags) and `bench/tools/ccdb.sh DB` (per entry: gcc-13 in the entry's
   directory with its arguments minus `-c -o X -M*` plus `-fsyntax-only`,
   against `cereal check --compile-commands DB FILE`; normalised
   diagnostics as `corp.sh` does, plus exit status; "N entries, M differ";
   then one full run at `-j12` for time and the summary).

Goldens: `tests/ccdb/*.cmd` (each writes its `compile_commands.json`, with
absolute directories made from `$PWD` and printed back as `<ROOT>`):
`arguments` and `command` forms; relative and absolute `file`; relative
`directory`; relative `-I` and `-include` resolved against the directory
(diagnostic names relative); two directories (the directory lines);
duplicate entries (one check) and entries differing in `-D` (two);
`.cpp`/`.S` entries (not C); an unknown option and `-std=c2x` (not checked,
aggregated message, exit 3); a missing file; FILES as a file, a directory,
and matching nothing (exit 2); `-Werror` in an entry (warning gives exit
1); cereal's `-Wall` after the entry's flags; an entry with `-MD -MF`
(no file written, checked with `ls`). run.sh section 7: replay at `-j1`
and `-j12` byte-identical.

Gates: `tests/run.sh` green; `verify.sh` unchanged (gcc.dg 3908/3910,
c-c++-common 635/636, cpp 285/285); sanitizer build clean, plus a
ThreadSanitizer run of the replay goldens at `-j12` (option parsing now
runs on worker threads); `ccdb.sh` on cereal's own sources (expected:
every entry clean, exit 0) and on libuv (cmake with
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`, Unix Makefiles; numbers recorded in
HISTORY as the baseline, not a target).

Lines (src, new or changed, moved code not counted): compdb.c ~80,
driver.c ~40, srcmgr/pp/par ~20, diag ~7, main.c ~210, config.c and LSP
display ~15: about 370, realistic 550 to 750.

### Slice 2: response files (DONE, Round 206; est. half a day, realistic 1)
Result: as designed, with these differences. `shell_split` became
`split_args(Arena *, s, SplitMode, &argv)` (the design's name); the entry
hook expands into `Options.rsp`, an arena that `options_free` releases,
because options keep pointers into their argument strings (`-D`, `-I`) and a
worker's `Options` dies with its job. Probes against gcc-13 corrected
section 5: a file holding only `''` gives one empty argument (only an empty
or white-space-only file gives none); a trailing backslash also gives an
empty argument; the limit is 1999 expansions (the 2000th, counting the
top-level one, is `too many @-files encountered`); any argument starting
with `@` is expanded wherever it stands (`-D @f` expands `@f`). gcc 15
changed libiberty's quoting (a backslash inside quotes is literal), so the
differential pins `gcc-13` (`RSPCC`). Gap, closed in Round 207:
`entry_is_c` did not see `-x` inside a response file (the language check
read the entry's own words, before expansion); it now expands first
(`tests/ccdb/rsp_lang`). The LSP never used `entry_is_c`. Tests: `tests/rsp/*.rsp` (18 files, run.sh 13b), `tests/ccdb/rsp*`.
Plan as designed:
`SPLIT_GCC`, `argv_expand` (read, split, splice, nested, limit,
directory and missing errors), hooks in `main` and `entry_options`.
Tests: run.sh section 13: a differential against `$REFCC` that passes
`-D` macros through response files (quotes, backslashes, nested,
whitespace-only, empty argument) and compares `-E -P` output; error cases
(missing, directory, self-recursive) with exit status; a `tests/ccdb`
case with an `@file` in an entry, relative to its directory. Lines about
75, realistic 110 to 150.

### Slice 3: `-M` family (est. 5 days, realistic 7.5 to 10)
The 6.6 probes are done (2026-10-10). Start with Options and validation, `IncludeEvent`
`dir_index`, the listener, the writer, `.d` naming, the deps-only mode,
`-MG`, `host_preinclude`; then `tests/deps`, run.sh section 15 and the
section 5 parallel case; then `deps.sh` on the corpora and the `par.py`
change. run.sh section 13 changes: `-MF`, `-MT`, `-MQ` alone become errors
(gcc's) and `-MD`/`-MMD -MP` now write files, so they leave the "skipped
options leave the output unchanged" loop and get their own cases.
Gates: run.sh; `verify.sh` with the four `missing-*` tests added (old and
new counts reported together); `deps.sh` numbers recorded; sanitizers.
Lines: Options and parsing ~90 (and ~25 deleted), listener ~90, writer
~70, naming ~40, mode plumbing ~45, `-MG` ~8, probe-host ~10: about 355,
realistic 530 to 710.

### Totals
About 800 lines of src est., 1,200 to 1,600 with a 1.5 to 2x allowance;
about 8.5 working days est., 13 to 17 realistic. Scripts and
tests come on top (~250 lines of scripts, ~40 small test files).

### Docs to update when each slice lands
HISTORY round entries; ROADMAP A4 status and the "Where it stands" list
(also its stale `-std=` line: A3 is done); STATUS "How to check it"
(`ccdb.sh`, `deps.sh`); LSP.md (entries resolve against their directory,
`.cereal` flags still rewritten); the `usage()` text; this file's results
section.

## 8. Open questions for the user
- Q1. Default `-std=gnu17` everywhere (recommended: build lines assume
  gcc's default; one mechanism), only for replay entries (smaller blast
  radius, but the CLI keeps judging build lines as strict C99), or keep
  C99.
- Q2. Exit 3 for "nothing failed but some entries were not checked"
  (recommended), or fold it into 1.
- Q3. No `cereal deps` mode (recommended), or an alias for `-MM`.
- Q4. A missing `@file` is an error (recommended: in an entry gcc's
  literal fallback silently drops flags), or gcc's literal behaviour.
- Q5. clang-built databases: unknown clang flags make the entry "not
  checked" (recommended for slice 1, keeps the A2 policy), or a
  `--lenient` that skips unknown flags as the LSP does.
- Q6. `-j` default stays `cpu_count()` (recommended: consistent with the
  other modes; scripts pass `-j12`), or three quarters of the cores.
- Q7. "Entering directory" lines on directory changes (recommended), or
  always print absolute names in replays (loses gcc-identical text).
- Q8. `.h` entries skipped as not C (recommended, consistent with A2's
  `-x c-header` rejection), or checked as C.

## 9. Review log
Three adversarial passes over the draft, each re-checking claims against
the code at `abc8183` and the probe output.
- Round 1 (claims vs code): `ignored_opts` is at `driver.c:123`, not 120;
  the fatal sites and the 17 `fprintf` sites recounted (correct). Found
  that run.sh section 1 runs `cereal -E` with the default standard against
  `$REFCC -std=c99` (`run.sh:42-43`), so D4 would break it silently: added
  to 3.3. Found a contradiction: 4.4 made cereal's own relative paths
  absolute "at the front of main", i.e. per-option rewriting, against D1;
  resolved by keeping `add_flag` as the one table of path options for
  arguments from a different base (cereal's own, `.cereal` files) and
  passing both through `entry_options`' extras (3.1, 3.5), which also
  fixed a hidden ordering bug (`.cereal` flags must be parsed before
  `options_finish`). The ten gcc `-M` tests were read: `cmdlne-M.c` and
  `cmdlne-M-2.c` had no home; now `tests/deps` cases (6.5). Removed two
  unverified claims (an `-E` parity statement in 3.1, a "1.5 to 2x seen on
  B1 to B3" in the totals) and a quoted gcc message I had not probed.
- Round 2 (internal consistency): the fingerprint excluded `IGN_SILENT`
  options but 6.2 moves `-c`/`-S` out of that class, so dedupe would have
  split on them: excluded explicitly. The directory lines were not said to
  be decided at commit time, which `-j` identity needs. Checked that each
  TU owns its `SrcMgr` (a per-TU `cwd` would be wrong with a shared one)
  and that no other path-keyed cache spans TUs; checked that the
  `index.c` `name` users outside the LSP are `index`/`query` only. Fixed
  two dangling section references and a redundant `TuJob` field.
- Round 3 (the brief's checklist item by item, and the open questions
  against the decisions): every listed topic has a section; Q1 to Q3
  match D4, D9, D8. Only finding: slice 1 step 1 did not list moving
  `add_flag`. A full pass after that found nothing material.

Unverified items of this log (phase A event order, the `-MD -MMD`
precedence, `-MQ` order beyond three, the old 6.6 list) were probed on
2026-10-10 against gcc-13 and the instrumented tree: 6.1, 6.6, 6.10. They
corrected two statements (the `-imacros` position, the `-MQ` order).

## 10. Probes behind sections 5 and 6.1
Run on WSL with gcc 13.4.0 in throwaway directories under `/tmp`; the
scripts were scratch files, not kept in the repo (slice 3 turns them
into `tests/deps` cases). They covered: `-M`/`-MM` with `-include`,
`-imacros`, `-isystem`, `#pragma GCC system_header`, absolute includes,
`-MG` with quoted and angled misses, `-MP`, `-MT`/`-MQ` quoting and order,
file names with `$ # space \ :`, wrapping at the 72-column boundary,
`-MD`/`-MMD` naming with and without `-o`, `-c`, `-E`, `-fsyntax-only`,
one and two inputs, errors and fatal errors, `-MF -`, `-MF` twice, `-M`
with `-o`, `-M` with `-fsyntax-only` and `-E`, duplicate spellings with
and without guards and `#pragma once`, trailing slashes in `-I`, the
preinclude with `-nostdinc`, `-ffreestanding`, `-undef`; response files
with every quoting form, nesting, whitespace-only, missing, directory and
self-recursive files; cereal's current default standard and `-M` handling.
