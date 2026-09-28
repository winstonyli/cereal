/* mgraph.h - the macro dependency graph (docs/PARALLEL.md, "Where the
 * macro graph comes in").
 *
 * Nodes are macro definitions (versions: every #define, every pop_macro
 * re-instatement).  Edges go from a definition to the *names* its
 * replacement list mentions; a name resolves to a definition only at the
 * version where the expansion happens, so everything version-dependent
 * takes a version.  Names that fail to resolve still count: defining them
 * later changes the result.
 *
 * A definition is *open* when ## may synthesize a name that no token
 * spells (an identifier or parameter on the left of ##).  A closure that
 * reaches an open definition says so instead of being silently
 * incomplete.
 *
 * The graph is built once all definitions are known (after the TU, or
 * after phase A) and is immutable afterwards: queries may run
 * concurrently. */
#ifndef CEREAL_MGRAPH_H
#define CEREAL_MGRAPH_H

#include "pp.h"

typedef struct MNode {
    Macro *m;
    struct Ident **names;     /* distinct, in first-use order */
    uint32_t nnames;
    bool open;                /* ## may form names not spelled anywhere */
} MNode;

typedef struct MacroGraph {
    PP *pp;
    MNode *nodes;             /* indexed by Macro.id */
    size_t n;
    /* reverse edges, CSR by Ident.id: definitions whose body names it */
    uint32_t *rev_start;      /* nidents + 1 entries */
    Macro **rev;
    size_t nidents;
} MacroGraph;

void mgraph_build(MacroGraph *g, PP *pp);
void mgraph_free(MacroGraph *g);

static inline const MNode *mgraph_node(const MacroGraph *g, const Macro *m)
{
    return m->id < g->n ? &g->nodes[m->id] : NULL;
}

/* Definitions (any version) whose replacement list names `name`. */
Macro *const *mgraph_users(const MacroGraph *g, const struct Ident *name,
                           size_t *n);

/* The dependency set of some text at a version: every name its expansion
 * can look up (transitively), and the definitions they resolve to. */
typedef struct MClosure {
    VEC(struct Ident *) names;   /* in discovery order; includes misses */
    VEC(Macro *) macros;         /* resolved definitions */
    bool open;                   /* an open definition was reached */
} MClosure;

void mgraph_closure(const MacroGraph *g, struct Ident *const *names,
                    size_t n, uint32_t version, MClosure *out);
void mclosure_free(MClosure *c);

/* Reusable marks for many closures in a row (one per thread). */
typedef struct MScratch {
    uint32_t *mark;
    size_t cap;
    uint32_t gen;
} MScratch;
void mgraph_closure_with(const MacroGraph *g, MScratch *s,
                         struct Ident *const *names, size_t n,
                         uint32_t version, MClosure *out);
void mscratch_free(MScratch *s);

/* Mutually recursive definitions at a version: strongly connected
 * components with more than one member (a direct self-reference is a
 * one-member component and is reported elsewhere).  Returns *ncomps
 * components, each an arena array of members in definition order,
 * terminated by NULL; components ordered by their first member. */
Macro ***mgraph_cycles(const MacroGraph *g, uint32_t version, Arena *a,
                       size_t *ncomps);

#endif
