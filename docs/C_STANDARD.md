# C language coverage

Measured 2026-10-21 by `tools/c_survey.py` (116 small probes, one feature each, every one a function
`int t(int k)`). Regenerate with `python3 -P tools/c_survey.py ./cereal` (Linux, needs gcc 15) and paste
the output below; it is a survey of the features listed, not a conformance suite, and the probe list is the
roadmap's source: add a probe when a feature is missing from it.

Columns: **gcc** whether gcc 15 accepts the probe (`probe` = it does not, so the row says nothing);
**front end** whether `cereal check` accepts it (a C23 row under `-std=gnu23`, the others under `-std=gnu17`); **back end** whether `--dump-asm` emits it
(`unsup: why` otherwise) and, where the probe is runnable, whether `tests/asm_exec.py` finds `t` equal to gcc's
(`ok, equals gcc`; `emits` = emitted but not run, because the probe needs a library or a static callee;
`DIFFERS` = see the notes). `-std=` is accepted for c99, gnu99, c11, gnu11, c17, gnu17, c2x, gnu2x, c23 and
gnu23 (2026-10-20, `docs/C23_PLAN.md`); c89, c90 and gnu89 are refused.

## Summary

111 of 114 meaningful probes pass the front end and 89 are emitted by the back end (all that can run equal
gcc's). Nothing from C89 through C17 fails the front end. The 3 front-end failures are C23 and parked:
`_BitInt`, `#embed` and `__has_embed`.

## Back-end gaps (front end accepts, no code)

- Types: variable-length arrays (`array type`), `_Complex`, `__int128`, `long double`, `_Float*`/`__float128`,
  decimal floats. These are `PARKED.md` row "T4b types".
- Statements and expressions: `case` ranges, nested functions, `__label__`, `_Generic` selecting an lvalue,
  `__builtin_choose_expr` of an lvalue (`lvalue form`).
- Built-ins: `alloca`, `frame_address`, `trap`, `object_size`, `__builtin_fabs`/`floor` (math). The
  overflow built-ins and `<stdckdint.h>` work (2026-10-20, `tests/asm_exec/ovf.c`), and so do the C23
  `__builtin_stdc_*` bit functions behind `<stdbit.h>` (2026-10-20, `tests/asm_exec/stdc.c`);
  an unsigned 64-bit operand mixed with a signed one, unsigned operands into a signed result and a 64-bit
  signed operand into an unsigned 64-bit result are reported unsupported (the exact value needs more than
  64 bits to test).
- Asm with operands in a template the generator does not recognize; static data initialized by a
  compound literal (`initializer expression`).

## `alias` and `ifunc`

A reference to a symbol declared with `__attribute__((alias(..)))` or `ifunc` is reported unsupported
(2026-10-20; it used to be ignored, leaving a link error). Emitting the alias is parked.

## Probes gcc 15 rejects

`implicit-int` (error since gcc 14) and `builtin-rotate` (`__builtin_rotateleft32` is clang's; gcc has none,
so the corpus uses of it are clang-only code, not a gap for a gcc-compatible build).

cereal accepts -std= c99, gnu99, c11, gnu11, c17, gnu17, c2x, gnu2x, c23, gnu23

| area | feature | since | gcc | front end | back end |
|---|---|---|---|---|---|
| C89 | digraphs | C95 | ok | ok | ok, equals gcc |
| C89 | iso646 | C95 | ok | ok | ok, equals gcc |
| C89 | kr-definition | C89 | ok | ok | emits |
| C89 | implicit-int | C89 | probe |  |  |
| C89 | enum-switch | C89 | ok | ok | ok, equals gcc |
| C89 | bitfields | C89 | ok | ok | ok, equals gcc |
| C89 | varargs | C89 | ok | ok | ok, equals gcc |
| C99 | vla | C99 | ok | ok | unsup: array type |
| C99 | vla-param | C99 | ok | ok | ok, equals gcc |
| C99 | vla-typedef | C99 | ok | ok | unsup: array type |
| C99 | flexible-array | C99 | ok | ok | ok, equals gcc |
| C99 | designated-init | C99 | ok | ok | ok, equals gcc |
| C99 | compound-literal | C99 | ok | ok | ok, equals gcc |
| C99 | compound-literal-static | C99 | ok | ok | unsup: gp: initializer expression 5 |
| C99 | inline | C99 | ok | ok | emits |
| C99 | restrict | C99 | ok | ok | ok, equals gcc |
| C99 | mixed-decls | C99 | ok | ok | ok, equals gcc |
| C99 | bool | C99 | ok | ok | ok, equals gcc |
| C99 | long-long | C99 | ok | ok | ok, equals gcc |
| C99 | complex | C99 | ok | ok | unsup: complex type |
| C99 | hex-float | C99 | ok | ok | ok, equals gcc |
| C99 | variadic-macro | C99 | ok | ok | ok, equals gcc |
| C99 | pragma-operator | C99 | ok | ok | ok, equals gcc |
| C99 | func-name | C99 | ok | ok | ok, equals gcc |
| C99 | ucn-identifier | C99 | ok | ok | ok, equals gcc |
| C99 | va-copy | C99 | ok | ok | ok, equals gcc |
| C99 | trailing-enum-comma | C99 | ok | ok | ok, equals gcc |
| C99 | float-env-rounding | C99 | ok | ok | emits |
| C11 | generic | C11 | ok | ok | ok, equals gcc |
| C11 | static-assert | C11 | ok | ok | ok, equals gcc |
| C11 | alignas | C11 | ok | ok | ok, equals gcc |
| C11 | noreturn | C11 | ok | ok | ok, equals gcc |
| C11 | thread-local | C11 | ok | ok | ok, equals gcc |
| C11 | atomic-qualifier | C11 | ok | ok | ok, equals gcc |
| C11 | stdatomic | C11 | ok | ok | ok, equals gcc |
| C11 | anonymous-members | C11 | ok | ok | ok, equals gcc |
| C11 | uchar | C11 | ok | ok | ok, equals gcc |
| C11 | generic-struct-ptr | C11 | ok | ok | unsup: lvalue form |
| C11 | quick-exit-header | C11 | ok | ok | emits |
| C23 | nullptr | C23 | ok | ok | ok, equals gcc |
| C23 | constexpr | C23 | ok | ok | ok, equals gcc |
| C23 | typeof | C23 | ok | ok | ok, equals gcc |
| C23 | auto-type | C23 | ok | ok | ok, equals gcc |
| C23 | bool-keyword | C23 | ok | ok | ok, equals gcc |
| C23 | attributes | C23 | ok | ok | ok, equals gcc |
| C23 | fallthrough-attr | C23 | ok | ok | ok, equals gcc |
| C23 | digit-separator | C23 | ok | ok | ok, equals gcc |
| C23 | binary-literal | C23 | ok | ok | ok, equals gcc |
| C23 | enum-underlying | C23 | ok | ok | ok, equals gcc |
| C23 | empty-initializer | C23 | ok | ok | ok, equals gcc |
| C23 | static-assert-no-message | C23 | ok | ok | ok, equals gcc |
| C23 | bitint | C23 | ok | FAIL expected ';' before 'b' |  |
| C23 | unnamed-parameter | C23 | ok | ok | emits |
| C23 | va-start-one-arg | C23 | ok | ok | ok, equals gcc |
| C23 | u8-char-literal | C23 | ok | ok | ok, equals gcc |
| C23 | alignas-keyword | C23 | ok | ok | ok, equals gcc |
| C23 | thread-local-keyword | C23 | ok | ok | ok, equals gcc |
| C23 | stdckdint | C23 | ok | ok | ok, equals gcc |
| C23 | stdbit | C23 | ok | ok | ok, equals gcc |
| C23 | unreachable | C23 | ok | ok | ok, equals gcc |
| C23 | label-before-declaration | C23 | ok | ok | ok, equals gcc |
| C23 | decimal-float | C23 | ok | ok | unsup: decimal floating type |
| C23 | floatN | C23 | ok | ok | unsup: _FloatN type |
| GNU | statement-expression | GNU | ok | ok | ok, equals gcc |
| GNU | typeof-gnu | GNU | ok | ok | ok, equals gcc |
| GNU | auto-type-gnu | GNU | ok | ok | ok, equals gcc |
| GNU | computed-goto | GNU | ok | ok | ok, equals gcc |
| GNU | label-table | GNU | ok | ok | ok, equals gcc |
| GNU | case-range | GNU | ok | ok | unsup: case range |
| GNU | conditional-omitted | GNU | ok | ok | ok, equals gcc |
| GNU | zero-length-array | GNU | ok | ok | ok, equals gcc |
| GNU | nested-function | GNU | ok | ok | unsup: statement FUNC_DEF |
| GNU | local-label | GNU | ok | ok | unsup: statement LOCAL_LABEL |
| GNU | int128 | GNU | ok | ok | unsup: __int128 type |
| GNU | long-double | GNU | ok | ok | unsup: value of a type outside the integer and float subset k20 |
| GNU | float128 | GNU | ok | ok | unsup: _FloatN type |
| GNU | complex-parts | GNU | ok | ok | unsup: complex type |
| GNU | range-designator | GNU | ok | ok | ok, equals gcc |
| GNU | empty-struct | GNU | ok | ok | ok, equals gcc |
| GNU | attr-packed | GNU | ok | ok | ok, equals gcc |
| GNU | attr-aligned | GNU | ok | ok | ok, equals gcc |
| GNU | attr-cleanup | GNU | ok | ok | ok, equals gcc |
| GNU | attr-alias | GNU | ok | ok | unsup: reference to the alias or ifunc symbol b |
| GNU | attr-weak | GNU | ok | ok | emits |
| GNU | attr-constructor | GNU | ok | ok | ok, equals gcc |
| GNU | attr-section | GNU | ok | ok | ok, equals gcc |
| GNU | attr-unused-used-noinline | GNU | ok | ok | emits |
| GNU | vector-size | GNU | ok | ok | ok, equals gcc |
| GNU | asm-statement | GNU | ok | ok | ok, equals gcc |
| GNU | asm-operands | GNU | ok | ok | unsup: asm template "movl %1,%0" with operands |
| GNU | named-variadic-macro | GNU | ok | ok | ok, equals gcc |
| GNU | extension-keyword | GNU | ok | ok | ok, equals gcc |
| GNU | builtin-choose-expr | GNU | ok | ok | unsup: lvalue form |
| GNU | builtin-overflow | GNU | ok | ok | ok, equals gcc |
| GNU | builtin-alloca | GNU | ok | ok | unsup: built-in __builtin_alloca |
| GNU | builtin-offsetof | GNU | ok | ok | ok, equals gcc |
| GNU | builtin-constant-p | GNU | ok | ok | ok, equals gcc |
| GNU | builtin-frame-address | GNU | ok | ok | unsup: built-in __builtin_frame_address |
| GNU | builtin-rotate | GNU | probe |  |  |
| GNU | builtin-mathfn | GNU | ok | ok | unsup: built-in __builtin_fabs |
| GNU | builtin-trap | GNU | ok | ok | unsup: built-in __builtin_trap |
| GNU | builtin-object-size | GNU | ok | ok | unsup: built-in __builtin_object_size |
| PP | has-include | C23 | ok | ok | ok, equals gcc |
| PP | has-attribute | GNU | ok | ok | ok, equals gcc |
| PP | has-builtin | GNU | ok | ok | ok, equals gcc |
| PP | has-c-attribute | C23 | ok | ok | ok, equals gcc |
| PP | va-opt | C23 | ok | ok | ok, equals gcc |
| PP | elifdef | C23 | ok | ok | ok, equals gcc |
| PP | embed | C23 | ok | FAIL invalid preprocessing directive #embed |  |
| PP | has-embed | C23 | ok | FAIL function-like macro "__has_embed" is not defined |  |
| PP | include-next | GNU | ok | ok | ok, equals gcc |
| PP | warning-directive | GNU | ok | ok | ok, equals gcc |
| PP | counter | GNU | ok | ok | ok, equals gcc |
| PP | pragma-pack | GNU | ok | ok | ok, equals gcc |
| PP | stdc-macros | C11 | ok | ok | ok, equals gcc |
| PP | line-file-date | C89 | ok | ok | emits |

116 probes: front end ok 111, back end ok 89, probes gcc rejects 2
