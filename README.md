# cereal

A C99 + GNU front end, written in C99: a GCC-compatible preprocessor with
deep static analysis of directives and macros, a parser, and a type checker
that reports gcc-13's diagnostics. It also builds an index that gives macros
the same editor features as variables and functions. It is a front end, not a
compiler: it parses and checks C and emits no code.

It targets Linux (x86-64) and is developed under WSL. It reads the host gcc's
headers and predefined macros at build time, so it needs a POSIX shell and gcc
(or another C compiler) to build; there is no native Windows or macOS support.

```
make            # probes the host compiler, builds ./cereal
make test       # differential + analysis + query tests
bench/fetch_corpus.sh /tmp/corpus && bench/corpus.py ./cereal /tmp/corpus
                # real code (Lua, libuv, zstd, Cython output) vs gcc
```

## Usage

```
cereal -E  [opts] file.c...           # preprocess (GCC-compatible output)
cereal lint [opts] file.c...          # macro / conditional / include analyses
cereal index [--all] file.c           # LSP model as JSON
cereal parse [--dump] file.c...       # C99 + GNU parser (also -fsyntax-only)
cereal query def|refs|hover|visible|expand|callers|callees|deps FILE:LINE:COL file.c
cereal lsp                            # language server (stdio), docs/LSP.md
cereal --list-warnings
```

Options: `-I -iquote -isystem -D -U -include -nostdinc -undef -std=c99|gnu99
-O<n> -pedantic -pedantic-errors -trigraphs -W<name> -Wno-<name> -W<group>
-Wall -Werror -Weverything -w -fdiagnostics-format=json -P -o`.

For the C front end (`cereal parse`, `-fsyntax-only`), `-W<name>` takes gcc's
warning names (about 180 are known; `--list-warnings` prints them), plus
`-fcf-protection`, `-ftrack-macro-expansion`, `-fshort-enums`,
`-fstrict-aliasing`, `-flax-vector-conversions` and the other flags that
change which diagnostics gcc emits. `-fsyntax-only` accepts `-std=c99` and
`-std=gnu99` only.

Several inputs are processed `-j N` at a time (default: all cores).
Output and diagnostics come out in input order, identical to `-j1`.
`-E` also preprocesses large files in parallel. It uses two phases: directives
first, then the text on all cores. The output is byte-identical to a
sequential run. See `-fparallel=auto|on|off`, `-fparallel-threads=N` and
docs/PARALLEL.md.

## Parity with gcc

The checker is tested against gcc-13's own testsuite: on the files it can run,
3720 of 3744 in gcc.dg and 633 of 636 in c-c++-common report the same
diagnostics as gcc (message, line and column). The testsuite is not part of
this repository (it is GPL); clone it yourself:

```
git clone --filter=blob:none --no-checkout --depth 1 --branch releases/gcc-13.3.0 \
    https://github.com/gcc-mirror/gcc ~/gccts
git -C ~/gccts sparse-checkout set gcc/testsuite/gcc.dg gcc/testsuite/c-c++-common
git -C ~/gccts checkout
make -j6 && CEREAL_GCC=gcc-13 bench/tools/verify.sh   # needs gcc-13, python3
```

Design: docs/PARSER.md, docs/TYPES.md. Current state and open differences:
docs/STATUS.md; the full log: docs/HISTORY.md.

## What lint finds

```c
#define SQUARE(x) x * x           // parameter 'x' is an operand of '*' but is not parenthesized
#define CHECK(c) if (!(c)) abort() // 'if' without 'else': dangling-else hazard
MAX(i++, j)                        // argument 1 has side effects and is evaluated 2 times
#ifdef FEATUR_A                    // 'FEATUR_A' is never defined; did you mean 'FEATURE_A'?
#elif VERSION > 3                  // after '#if VERSION > 2': branch can never be taken
#ifndef FOO_H / #define FOO_HH     // header guard mismatch
```

Groups: `hygiene`, `cond`, `include`, `pp`. See `--list-warnings`.

## Design

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). In short:

- A streaming lexer over one reserved address region (`text = region +
  loc`), 16-byte pointer-free tokens, and GCC/Clang-style context-stack
  expansion with recycled buffers, so memory stays flat on huge generated files.
- Analyzers and the index are listeners on preprocessor events. A static
  per-file skeleton also covers inactive code.
- Host headers are used as they are: `tools/probe-host.sh` captures GCC's
  C99 include path, predefined macros and `__has_attribute`/`__has_builtin`
  answers at build time.
- Conformance is tested by token-level diffs against `gcc -std=c99 -E` on the
  C99 standard's examples, all C99 and common POSIX headers, and cereal's
  own sources.

## AI-assisted development

Most of this code was written with Claude (Anthropic) under my direction:
265 of the 266 commits carry a `Co-Authored-By: Claude` trailer, and 47 are
authored by Claude directly. I set the goals, chose gcc-13 as the reference,
reviewed the results, and kept the verification honest: every behaviour is
checked against real gcc output, and each fix leaves a golden test in
`tests/check`.

## License

MIT, see [LICENSE](LICENSE).
