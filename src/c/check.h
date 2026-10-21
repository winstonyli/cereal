/* check.h - the semantic checker: types, declarations and constant
 * expressions of the units the parser produces (docs/TYPES.md).
 *
 * One pass over each unit's post-order node array, in order, with side
 * arrays per node (type, constant value).  Declarations with file scope
 * persist from unit to unit; everything else is freed at the end of the
 * unit.  Diagnostics follow gcc's wording and locations. */
#ifndef CEREAL_CHECK_H
#define CEREAL_CHECK_H

#include "c/ast.h"
#include "c/target.h"
#include "diag.h"
#include "intern.h"
#include "srcmgr.h"

typedef struct Checker Checker;

typedef struct CheckOptions {
    const Target *target;
    bool gnu;                   /* -std=gnu99 ... */
    int std_year;               /* 1999, 2011, 2017 or 2023 */
    bool pedantic;
    bool pedantic_errors;
    bool short_enums;           /* -fshort-enums */
    char opt_level;             /* -O level: 0 or '0': none (-O0) */
    char strict_alias;          /* -fstrict-aliasing 1, -fno-strict-aliasing 2 */
    bool lax_vector;            /* -flax-vector-conversions */
    bool optimize;              /* -O1 and up */
    bool cf_nobranch;           /* -fcf-protection=none|return: nocf_check is ignored */
    FILE *dump;                 /* --dump-types: declarations and layouts */
    FILE *tast;                 /* --dump-tast, --dump-cfg: the typed tree of each unit */
    bool tast_tree;             /* ... print the tree itself (--dump-tast) */
    bool dump_asm;              /* --dump-asm: x86-64 assembly of each function (cgx86.c) */
    const char *emit_obj;       /* --emit-obj=FILE: the same, as an ELF object (mir_elf.c) */
    bool eval_main;             /* --eval-main: run main() in the evaluator (eval.c) */
    bool eval_wrap;             /* ... with signed overflow wrapping (gcc -fwrapv) */
    bool dump_cfg, verify_cfg;  /* --dump-cfg: print the lowered control-flow graph;
                                 * --verify-cfg: and check it (tcfg.c) */
    bool verify_tast;           /* --verify-tast: check each tree, print findings */
    unsigned *tast_bad;         /* ... incremented per finding (NULL: not counted) */
    bool summaries;             /* keep per-unit summaries (csum.h) */
    FILE *dump_summaries;       /* --dump-summaries: print them (implies) */
    const char *validate_summaries; /* --validate-summaries=FILE: check each
                                   unit's recorded read set on entry */
    /* The macros a token spelled at `spelled` (a replacement-list token)
     * was expanded through, innermost first, ending with the invocation
     * at `exp`; 0 if the chain is unknown (pp.c pp_macro_chain). */
    size_t (*macro_chain)(void *ctx, SrcLoc spelled, SrcLoc exp,
                          MacroNote *out, size_t max);
    void *macro_ctx;
    /* Calls cb for each user macro defined at this point (spelling
     * candidates); NULL if unknown. */
    void (*macro_names)(void *ctx, void (*cb)(void *, const char *, size_t),
                        void *arg);
    void *macro_names_ctx;
    bool symidx;                /* build the C symbol index (csymidx.h) */
    FILE *symidx_verify;        /* --verify-symbols: report missed names */
    uint64_t *digest;           /* non-NULL: mix each unit's AST digest in (cdigest.c) */
} CheckOptions;

/* The x86-64 back end runs when assembly or an object is asked for. */
#define CHK_ASM(o) ((o).dump_asm || (o).emit_obj)

Checker *checker_new(SrcMgr *sm, Interner *in, DiagEngine *diag,
                     const CheckOptions *opt);
/* Checks one unit.  had_errors: the parser reported syntax errors in it
 * (its declarations are still recorded, its diagnostics suppressed). */
void checker_unit(Checker *c, const ParseUnit *u, bool had_errors);
/* The end of the translation unit (tentative definitions, unused static
 * functions, ...). */
void checker_finish(Checker *c);
/* The C symbol index (CheckOptions.symidx), after checker_finish; the
 * caller frees it.  NULL without one. */
struct CIndex *checker_take_index(Checker *c);
void checker_free(Checker *c);

#endif
