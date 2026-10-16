/* bdd.h - reduced ordered binary decision diagrams (presence conditions).
 * Variable i is above variable j when i < j.  A node cap bounds the memory
 * of one manager: past it every operation returns BDD_FALSE and `over` is
 * set, so a caller checks `m->over` once after a batch and treats the
 * result as unknown. */
#ifndef CEREAL_BDD_H
#define CEREAL_BDD_H

#include "../common.h"

typedef uint32_t Bdd;
#define BDD_FALSE 0u
#define BDD_TRUE 1u

typedef struct BddNode {
    uint32_t var;       /* UINT32_MAX for the two terminals */
    Bdd lo, hi;
} BddNode;

typedef struct BddCache {
    Bdd f, g, h, r;     /* f == 0 marks an empty slot: ite never memoizes it */
} BddCache;

typedef struct BddMgr {
    VEC(BddNode) nodes;
    Bdd *table;         /* open addressing, 0 = empty (terminals are not keyed) */
    size_t tcap;
    BddCache *cache;    /* direct-mapped ite memo */
    size_t ccap;
    size_t cap;         /* maximum node count */
    bool over;
} BddMgr;

void bdd_init(BddMgr *m, size_t cap);
void bdd_free(BddMgr *m);
Bdd bdd_var(BddMgr *m, uint32_t v);
Bdd bdd_not(BddMgr *m, Bdd f);
Bdd bdd_and(BddMgr *m, Bdd f, Bdd g);
Bdd bdd_or(BddMgr *m, Bdd f, Bdd g);
Bdd bdd_ite(BddMgr *m, Bdd f, Bdd g, Bdd h);
/* The variables f depends on, each once, in no particular order. */
typedef VEC(uint32_t) BddVars;
void bdd_vars(const BddMgr *m, Bdd f, BddVars *out);
bool bdd_eval(const BddMgr *m, Bdd f, const bool *assign);
/* The same, with the value of variable v given by val(ctx, v). */
bool bdd_eval_fn(const BddMgr *m, Bdd f, bool (*val)(void *ctx, uint32_t v),
                 void *ctx);
size_t bdd_size(const BddMgr *m);

/* `cereal selftest bdd`: ite/and/or/not against truth tables, the node cap.
 * Returns 0 when everything holds. */
int bdd_selftest(void);

#endif
