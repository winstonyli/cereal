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
    bool gnu;                   /* -std=gnu99 */
    bool pedantic;
    bool pedantic_errors;
    bool short_enums;           /* -fshort-enums */
    bool lax_vector;            /* -flax-vector-conversions */
    FILE *dump;                 /* --dump-types: declarations and layouts */
    bool summaries;             /* keep per-unit summaries (csum.h) */
    FILE *dump_summaries;       /* --dump-summaries: print them (implies) */
    const char *validate_summaries; /* --validate-summaries=FILE: check each
                                   unit's recorded read set on entry */
} CheckOptions;

Checker *checker_new(SrcMgr *sm, Interner *in, DiagEngine *diag,
                     const CheckOptions *opt);
/* Checks one unit.  had_errors: the parser reported syntax errors in it
 * (its declarations are still recorded, its diagnostics suppressed). */
void checker_unit(Checker *c, const ParseUnit *u, bool had_errors);
/* The end of the translation unit (tentative definitions, unused static
 * functions, ...). */
void checker_finish(Checker *c);
void checker_free(Checker *c);

#endif
