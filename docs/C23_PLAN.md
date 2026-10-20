---
status: draft
---

# C23: `-std=c23`, `c2x`, `gnu23`

Goal: the C23 language features of `docs/C_STANDARD.md` (11 of the 13 front-end failures there are
C23 syntax; the 12th and 13th, `#embed` and `__has_embed`, are parked below). Nothing here changes a
`-std` that is accepted today.

## Reference and non-goals

cereal is a gcc-13 clone (`src/driver.c`, `src/gcc_dumps.h`, the gate corpora). C23 mode is **the gcc-13
front end plus the C23 language features, checked against gcc 15** (the survey's oracle; gcc 13 has no
`#embed`, `_BitInt`, `<stdbit.h>`, `u8` character constants). Goldens for C23 files come from gcc 15 and
hold only diagnostics gcc 13 would also give for the construct. Non-goals: gcc 14/15 changes to older
modes (implicit declarations and pointer-type mismatches became errors in 14), `__GNUC__` stays 13.

## Mechanism (one, not one per feature)

A language year, 2023, in the places that already carry 1999/2011/2017:

1. **Names and macros.** `std_names` gets `c23`, `c2x`, `iso9899:2024`, `gnu23`, `gnu2x` (year 2023);
   `__STDC_VERSION__` is `202311L`; for year >= 2023 the predefines `__CHAR8_TYPE__` and
   `__GCC_ATOMIC_CHAR8_T_LOCK_FREE` join (the whole difference of gcc 15's `-dM` between gnu17 and gnu23,
   probed 2026-10-20). Update the `-std=` help and the `std_year` comments (`driver.h`, `check.h`).
2. **Keywords gated by year.** `ckw.h` flags `CKW_GNU_ONLY` and the new `CKW_C23` (live when the year is
   2023, whatever the gnu bit); the parser carries the year next to `p->gnu` (`ckw_of`, `parse.c:169`).
   The C23 spellings are aliases of keywords that exist: `static_assert`, `thread_local`, `alignas`,
   `alignof`, `typeof`, plus `typeof_unqual` (new). Below 2023 they stay identifiers, as now.
3. **The lexer** reads `'` digit separators and `u8'x'` (an `unsigned char`) when the year is 2023
   (`lex.h` options beside `uliterals`).
4. **Pedantic notes** (`pedwarn_c2x`, `check.c:172`) already compare the year, so a C23 file stops
   warning about binary constants, `[[]]`, empty initializers and the rest with no change.

## Features that need more than a flag

| Feature | What it takes | Risk |
|---|---|---|
| `bool`, `true`, `false` | keywords: `_Bool`, and constants of type `bool` | low |
| `static_assert(c)` | the message becomes optional | low |
| `va_start(ap)` | none: gcc's `<stdarg.h>` defines it by `__STDC_VERSION__` | low |
| `()` means `(void)` | a year check where an empty list makes a no-prototype function type | medium: reaches calls, redeclarations, K&R-style definitions |
| `constexpr` | `const` plus a constant initializer, usable in constant expressions | medium |
| `nullptr`, `nullptr_t` | a type of its own (conversions, `_Generic`, `typeof`), lowered to a null pointer | high: new type kind |

## Parked (rows added to `PARKED.md` with this plan)

`_BitInt(N)` (a type of any width; the back end would refuse it), `#embed` and `__has_embed`
(a preprocessor directive), `-std=c89`/`gnu89` (separate: dropping C99 features).

## Order and checks

1. Mechanism items 1 to 4, keywords that are aliases, `bool`/`true`/`false`, one-argument
   `static_assert`. Check: every C23 probe of the survey that is not in the table above turns `ok`
   when run under `-std=c23`; `tests/run.sh` stays at its count for everything else; a keyword in a
   mode below 2023 is still an identifier (negative probes: `int bool, true;` under `-std=c17`).
2. `()` as `(void)`, `constexpr`.
3. `nullptr`.

After each: `tests/run.sh`, the zstd, Lua and libuv runs, the gate. New tests: `tests/check/c23_*.c`
with `// flags: -std=c23` and gcc-15 goldens, and the survey's C23 rows run under `-std=c23`
(`tools/c_survey.py`) with its back-end check against `gcc -std=c23`. Mutation check: drop the year from
one keyword and see the golden fail. Regenerate `docs/C_STANDARD.md`.

## Stress-test log

- Round 1 (against the code): the year is not known to the preprocessor or lexer, only to the checker
  and driver, so item 3 adds an option rather than reading a field; `pedwarn_c2x` needs no change;
  `std_year ==` comparisons do not exist (grep, 2026-10-20), so a new year breaks nothing; keywords must
  stay identifiers below 2023 (negative probes added to the checks); `()` as `(void)` was missing from
  the first list.
- Round 2 (against the oracle): gcc 15's `-std=c23` differs from `gnu23` only by `__STRICT_ANSI__`,
  `linux` and `unix`, which the gnu bit already handles; mixing a gcc-13 front end with gcc-15 goldens
  needs the goldens rule above or older-mode diagnostics will not match; `nullptr` is the one item that
  can stay behind without blocking the rest.
- Round 3: nothing material found.
