# cereal

A C99 toolchain, written in C99, whose first milestone is a conforming
preprocessor with deep static analysis of directives and macros. It also
builds an index that gives macros the same editor features as variables and
functions.

```
make            # probes the host compiler, builds ./cereal
make test       # differential + analysis + query tests
```

## Usage

```
cereal -E  [opts] file.c              # preprocess (GCC-compatible output)
cereal lint [opts] file.c...          # macro / conditional / include analyses
cereal index [--all] file.c           # LSP model as JSON
cereal query def|refs|hover|visible|expand FILE:LINE:COL file.c
cereal --list-warnings
```

Options: `-I -iquote -isystem -D -U -include -nostdinc -undef -std=c99|gnu99
-O<n> -pedantic -trigraphs -W<name> -Wno-<name> -W<group> -Wall -Werror
-Weverything -fdiagnostics-format=json -P -o`.

`-E` preprocesses large files in parallel. It uses two phases: directives
first, then the text on all cores. The output is byte-identical to a
sequential run. See `-fparallel=auto|on|off`, `-fparallel-threads=N` and
docs/PARALLEL.md.

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
