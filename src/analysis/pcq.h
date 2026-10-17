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

typedef enum { PQ_CONFIG, PQ_OPAQUE } PcReqKind;
typedef enum { PQ_MET, PQ_UNMET, PQ_UNKNOWN } PcReqStatus;

/* One macro (or one opaque text): what the cube needs of it. */
typedef struct PcReq {
    const char *clause;     /* "defined(X) && X >= 3"; owned by the cube */
    PcReqKind kind;
    PcReqStatus status;
} PcReq;

typedef struct PcCube {
    PcReq *reqs;
    uint32_t n;
    uint32_t cost;          /* the unmet CONFIG requirements */
} PcCube;

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
/* "defined(X) && X >= 3 [met]; ..." */
char *pc_cube_render(const PcCube *c, bool only_unmet);
/* The macros (and opaque texts) of f's support on which v1 and v2 differ and
 * that decide f: swapping one alone changes f from its value under v1 to its
 * value under v2, or, if none does alone, all that differ.  Empty when f has
 * the same value under both.  Names are sorted; free the array with free(). */
size_t pc_diff(PcCtx *cx, Bdd f, const PcValuation *v1, const PcValuation *v2,
               const char ***names);

/* `cereal selftest pcq`: the cost DP against brute-force enumeration. */
int pcq_selftest(void);

#endif
