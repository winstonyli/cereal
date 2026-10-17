/* pcq.h - queries on presence conditions (docs/CONFIG_DESIGN.md section 4).
 *
 * A valuation V says what each initial atom is in the configuration at hand
 * (or that it is not known: an opaque atom with no pinned value).  The cost of
 * making a condition true is the number of macros (blocks of atoms, pcexpr.h)
 * whose assignment must differ from V: raising X from 1 to 5 is one change.
 * `preconditions` returns the cheapest ways, each as the requirements that
 * differ from or agree with V. */
#ifndef CEREAL_PCQ_H
#define CEREAL_PCQ_H

#include "pcexpr.h"

typedef struct PcValuation {
    void *ctx;
    /* The atom's value; *known false: it is free (no pin). */
    bool (*get)(void *ctx, const PcAtom *at, bool *known);
} PcValuation;

typedef enum { PQ_CONFIG, PQ_TARGET, PQ_ENV, PQ_OPAQUE, PQ_LANG } PcReqKind;
typedef enum { PQ_MET, PQ_UNMET, PQ_UNKNOWN } PcReqStatus;

/* One macro (or one opaque text): what the cube needs of it. */
typedef struct PcReq {
    const char *clause;     /* "defined(X) && X >= 3"; owned by the cube */
    PcReqKind kind;
    PcReqStatus status;
    Ident *id;              /* the macro it is about, if it is about one */
    bool residual;          /* it asks for a leaf the table has no name for */
    char *word;             /* config: the command-line word that sets it
                             * (-DX=3, -UX), owned by the cube */
    int axis, leaf;         /* language: the family axis and the leaf asked for
                             * (-1: only ruled leaves out) */
} PcReq;

typedef struct PcCube {
    PcReq *reqs;
    uint32_t n;
    uint32_t cost;          /* the unmet CONFIG requirements */
    uint32_t lang;          /* the unmet LANG requirements (a standard or mode
                             * change: no overlay does it) */
    uint32_t target;        /* the unmet TARGET requirements (an OS or compiler
                             * change: no overlay does it) */
    bool residual;          /* an unmet requirement is residual */
} PcCube;

/* the cube's cost as the one scalar the DP minimizes */
uint32_t pc_cube_weight(const PcCube *c);

/* f holds under V (unknown atoms count as false). */
bool pc_holds(PcCtx *cx, Bdd f, const PcValuation *v);
/* No opaque atom in f's support. */
bool pc_exact(PcCtx *cx, Bdd f);
/* The cheapest cubes making f true, cheapest first, at most k (the
 * guaranteed one is the first; the others are the next found).  Returns the
 * count; 0 when f is unsatisfiable or the BDD outgrew its cap.  Free each
 * with pc_cube_free. */
size_t pc_preconditions(PcCtx *cx, Bdd f, const PcValuation *v, size_t k,
                        PcCube *out);
void pc_cube_free(PcCube *c);
/* The command-line words (-DX=3 -UY -std=c11, space separated, appended to
 * sb) that make every unmet requirement of the cube hold on top of the unit's
 * own flags, whose present state is v.  False: the cube needs more than an
 * overlay (a target, environment or opaque change, an unknown, a standard
 * cereal does not read); sb is then unspecified. */
bool pc_enable(PcCtx *cx, const PcCube *c, const PcValuation *v, StrBuf *sb);
/* "defined(X) && X >= 3 [met]; ..." */
char *pc_cube_render(const PcCube *c, bool only_unmet);
/* The macros (and opaque texts) of f's support on which v1 and v2 differ and
 * that decide f: swapping one alone changes f from its value under v1 to its
 * value under v2, or, if none does alone, all that differ.  Empty when f has
 * the same value under both.  Names are sorted; free the array with free(). */
size_t pc_diff(PcCtx *cx, Bdd f, const PcValuation *v1, const PcValuation *v2,
               const char ***names);

/* Rank 1 of pc_preconditions against enumerating every assignment of the
 * constrained condition's support: 0 agrees, 1 differs, -1 not checked (over
 * 20 variables, unsatisfiable, node cap). */
int pc_check_rank1(PcCtx *cx, Bdd f, const PcValuation *v);
/* The least weighted cost of any satisfying assignment of g; false when the
 * support has more than maxvars variables. */
bool pc_cost_brute(PcCtx *cx, Bdd g, const PcValuation *v, size_t maxvars,
                   uint32_t *best);

/* `cereal selftest pcq`: the cost DP against brute-force enumeration. */
int pcq_selftest(void);

#endif
