# Roadmap: toolchain parity

Written 2026-10-08 after surveys of the LSP, the driver and C11 coverage.
Goal: macro tooling stays the unique selling point, and cereal reaches rough
parity with other C toolchains where it counts (editor, build integration,
compile commands). Numbers marked "est." are survey estimates, not
measurements. The gcc-13 parity work is at a plateau (STATUS.md).

## Where it stands
- Driver: `-E`, `-fsyntax-only`/`check`, `parse`, `lint`, `index`, `query`,
  `lsp`. No `-c`/`-S`, no code generator, no `-M*`, no response files.
- `-std=` accepts only `c99` and `gnu99`; anything else is fatal.
  `__STDC_VERSION__` is hard-coded to 199901L (`tu_begin`, driver.c).
- Unknown options are fatal (`-c -g -fPIC -m64 -MD -x -pthread -Wl,..`).
  `src/lsp/config.c` already tolerates and skips them for compile_commands
  (its own `takes_value` list), so the logic exists twice in part.
- In gnu99 mode the parser and checker already handle nearly all of C11
  (`_Generic`, `_Static_assert`, `_Alignas`, `_Atomic`, `_Noreturn`,
  `_Thread_local`, anonymous members, `u8`/`u`/`U` literals, host
  `<stdatomic.h>`, `<threads.h>`, ...). Gaps found: (`__VA_OPT__` was missing
  until Round 176, a `-E` fidelity bug in every mode: output kept the
  token); `__STDC_VERSION__` makes glibc hide C11 declarations
  (`timespec_get`); C2X keywords (`bool`, `nullptr`, `constexpr`, `u8'a'`).
- LSP: macro-only navigation. It never parses or checks, so it publishes no
  type diagnostics and nothing for functions, variables or types.
- Speed (cold WSL, rough): `check` on uvloop `loop.c` (203k lines) 1.0 to
  1.1 s against 0.5 to 0.9 s for `index`; on a 1.5k-line file 60 ms.

## Track A: command line usable on real build lines
Scope, stated precisely: cereal as a **checker driven by compile_commands or
a build line**, not a compiler replacement. It produces no objects, so a
build that needs `.o` files still needs gcc. `-c`/`-S` mean "check only".

A1. `__VA_OPT__` (DONE, Round 176; was est. half a day). gcc-13: available in gnu modes, a
    pedwarn "not available until C2X" under `-pedantic` in strict ISO modes
    (probed with `-std=c99` and `-std=c11`). Do first: it breaks `-E` output
    today, independent of `-std`. Check `#__VA_OPT__` against gcc-13 before
    implementing it.
A2. Shared flag handling (est. 2 days). Hoist the LSP `takes_value`/skip
    logic into driver.c as one function used by the CLI and `config.c`.
    Policy, from the review below:
    - An explicit **allowlist** of ignored flags, not a wildcard. Benign:
      `-c -S -pipe -g* -fPIC -fPIE -ffunction-sections -fdata-sections
      -fno-omit-frame-pointer -fstack-protector* -fvisibility=* -pthread
      -shared -static -rdynamic -pie -no-pie -Wl,* -Wa,* -l* -L*
      -march=* -mtune=*`. Value-taking: `-MF -MT -MQ -Xclang
      -Xpreprocessor -Xlinker -Xassembler -arch -target -idirafter -imacros
      -iprefix -iwithprefix -T -u -z -aux-info -dumpdir -dumpbase`.
    - Flags that **change front-end meaning** are never silently ignored:
      `-ffreestanding -fno-builtin -fsigned-char -funsigned-char -fwrapv
      -fno-common -fpack-struct -fms-extensions -fshort-wchar -m32/-m64`.
      Each is either implemented (`-m32`/`-m64` pick the target preset;
      `-fsigned-char` etc. change `char`) or reported once as "ignored: may
      change diagnostics" on stderr, with a count in the exit summary. Which
      are implemented is decided per flag by probing gcc-13.
    - `-x c` accepted, `-x c-header` and `-x c++` rejected with a clear
      message. `-isysroot`/`--sysroot` change include search. Unknown flags
      that are not on any list stay "unknown option".
    - `-MD -MMD -MP -MF -MT -MQ` consume their arguments. Writing the
      dependency file is track A4. Until then they are ignored with a note,
      because an absent `.d` file is a silent failure for ninja/make users.
A3. `-std=c11 c1x gnu11 gnu1x c17 c18 gnu17 gnu18 iso9899:2011 iso9899:2017
    iso9899:2018` (est. 1 to 2 days).
    - New `Options.std_year` (99, 11, 17) beside the existing gnu flag.
    - `__STDC_VERSION__` 201112L or 201710L. Diff `gcc-13 -dM -E` across all
      std values to find every predefined macro that differs, not only this
      one (`__STRICT_ANSI__`, `linux`/`unix`, others).
    - Strict modes (`c11`, `c17`) mirror what `c99` does today: no GNU
      extensions, trigraphs on, `u8`/`u`/`U` on (C11 has them), `::` not a
      token.
    - Pedantic text: `cped11` (check.c) currently warns "ISO C99 does not
      support X" for C11 features. Under C11 it must stay silent, and the
      remaining pedwarns need the right edition in their text. Probe every
      message against gcc-13 per std; do not assume.
    - `c2x`/`gnu2x`/`c23` stay fatal until C2X keywords are a separate
      slice. The LSP's `-std` hack (map unknown to gnu99) is deleted once
      A3 lands.
    - Effect on the parity gate: many gcc.dg tests use `-std=c11`/`gnu11`
      and are skipped today. Supporting them adds files to the "run" count,
      so parity will drop at first. That is test-suite growth, to be read as
      new work, not a regression. Report the old and new counts together.
A4. Response files `@file`, `-M*` output with `cereal deps` (the include
    graph and `query deps` exist; est. 1 week for the full -M family) and
    `cereal check --compile-commands DB [FILES]` that replays entries, with
    a non-zero exit when any file has errors (est. 3 days).
A5. SARIF output (`--format=sarif`) next to the existing JSON.

Verification for A: goldens for each std and each flag class; a harness
that runs the flag allowlist through gcc-13 `-fsyntax-only` and cereal and
compares exit code and diagnostics; the five-header `-E` probe against
`gcc -E -std=gnu11`; replay of libuv and zstd compile commands; `verify.sh`
extended with c11/gnu11/c17 runs.

## Track B: language server for ordinary C
Survey result: feasible, the planned P6 milestone in PARSER.md. The server
does not parse or check today. The checker resolves every identifier and
type but discards the result at the end of each unit.

B1. Run parse and check in the builder as a second phase after the macro
    snapshot publishes, with the existing cancel flag, and publish compiler
    diagnostics (est. 150 to 250 lines; lift `parse_one` out of main.c into
    a shared function). Cost spike above: about 60 ms on small files, about
    1 s on the largest unit. Small files have no cell-regenerated tokens, so
    this needs a second preprocessor pass for them (not yet measured).
B2. A C symbol index recorded in the checker: uses and declarations with
    location, kind, a stable declaration id, hover text copied out as a
    string before the checker is freed (est. 400 to 600 lines, 6 to 8 hook
    sites in cexpr.c, cspec.c, cstmt.c, cdecl.c, crecord.c, check.c).
    Gives definition, declaration, references, hover with types,
    documentHighlight and rename for functions, variables, parameters,
    typedefs, enumerators, fields, labels and tags. Macro lookup stays
    first. Rename refuses uses inside macro expansions and inactive `#if`
    branches.
B3. Document symbols, in-scope completion, signature help for C functions,
    typeDefinition, richer semantic tokens (est. 350 lines).
B4. Function call hierarchy, inlay hints (est. 400 lines).
B5. Workspace-wide index over compile_commands, workspace/symbol,
    cross-file references, on-disk cache. Large; needs its own design.

Risks: partial or erroneous code still yields partial symbol data; the
snapshot sees one `#if` branch; the checker's tables are private (index code
lives in `src/c/`); the 830 MB LSP peak noted in PARSER.md grows with a
retained index on huge files, so B1/B2 skip units above a size threshold.

## Track C: macro tooling (the selling point)
- `cereal fix` applying the diagnostics' fix-its, macro-to-inline/enum/const
  suggestions, a baseline file for CI (est. 3 to 5 weeks).
- Macro notes that match gcc ("in definition of macro" for tokens a
  parameter substituted): needs per-token provenance; design in HISTORY
  (after Round 175).

## Track D: later, larger
- Control-flow graph and dataflow for `-Wuninitialized`,
  `-Wmaybe-uninitialized`, `-Wimplicit-fallthrough`, `-Wnull-dereference`
  (est. 4 to 8 weeks). Under `-fsyntax-only` gcc-13 does not report the
  uninitialized-variable warnings either (they come from the optimizer), so
  this is parity with a compile, not with the current oracle.
- Back end: typed-AST export, x86-64 SysV classification, then QBE and `cc`
  for assembling (est. 8 to 12+ weeks, high risk). Nothing in the tree
  lowers the AST today. Start only after A and B.
- Not planned: formatter, sanitizer instrumentation, `-fanalyzer`.

## Order
1. A1, A2, A3 (about a week). They make cereal usable from compile_commands
   and unlock C11 code.
2. B1, B2 (about two weeks). Real C navigation and editor diagnostics.
3. A4, then C and B3.
4. D when there is appetite.
Each step gets a HISTORY entry and goldens as usual.

## Review log
Round 1 (self, against the surveys) found and fixed: the plan claimed a
"drop-in build wrapper" although no objects are produced (scope now stated);
a wildcard ignore for `-f*`/`-m*` would hide flags that change diagnostics
(`-ffreestanding`, `-fsigned-char`, `-m32`; allowlist and "ignored" note);
ignoring `-MD` silently breaks dependency tracking (A2 note, A4); supporting
c11 changes the parity gate's denominator (A3 note); the survey's
"`__VA_OPT__` in all modes" was imprecise (pedwarn in ISO modes, probed);
`__STDC_VERSION__` is not the only std-dependent predefine (A3 diff of
`-dM -E`); `-m32` needs i386 host headers that are not installed here, so it
selects the target but cannot be run end to end.
Round 2 (re-read for contradictions and gaps) found: `-x` handling had no
rule for `c-header`; the LSP `-std` hack removal was not tied to A3; both
fixed above. Nothing else material.
