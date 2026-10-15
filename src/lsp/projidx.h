/* projidx.h - the project index in memory (docs/B5_DESIGN.md 10.1, 10.2): the
 * records the live manifests name, each with the list of its names for
 * workspace/symbol.  The scheduler thread publishes (pi_set_tu); any thread
 * may read between pi_rdlock and pi_unlock.  Nothing in a PiRec is valid
 * outside the lock: a publish may release it. */
#ifndef CEREAL_PROJIDX_H
#define CEREAL_PROJIDX_H

#include "pstate.h"

enum {
    PIF_MACRO = 1,               /* a macro definition (kind is unused) */
    PIF_DEF = 2,                 /* the defining declaration */
    PIF_EXT = 4,                 /* a function or object with external linkage */
    PIF_FUNC = 8                 /* a function-like macro */
};

typedef struct PiSym {           /* 24 bytes */
    const char *name, *container;    /* into the record's mapping */
    uint32_t off;
    uint16_t len;
    uint8_t kind, flags;         /* CIK_*, PIF_* */
} PiSym;

struct PiKeyNode;
typedef struct PiRec {
    void *next;                  /* PiMap's chain; must stay first */
    uint64_t digest;             /* the key; must stay second */
    const char *path;            /* the file the record describes */
    uint64_t fhash;              /* cindex_hash of the text it was built from */
    PiSym *syms;
    uint32_t nsyms, refs;        /* refs: the units that list it */
    size_t slot;                 /* in the dense list */
    struct PiKeyNode *kn;        /* one per key of the record, in the key map */
    CRecMap map;
} PiRec;

typedef struct ProjIndex ProjIndex;

ProjIndex *pi_new(void);
void pi_free(ProjIndex *pi);

/* Scheduler thread only.  Makes tu.s records the ones its manifest names
 * (none if it has no manifest), loading those not yet in.  A record that
 * cannot be read is left out and reported to ps (pstate_reject_rec: every unit
 * that lists it becomes PS_LOST), and tried again at the next call. */
void pi_set_tu(ProjIndex *pi, PState *ps, const PTu *tu);
/* Forgets the units ps no longer has. */
void pi_prune(ProjIndex *pi, const PState *ps);

/* Readers. */
void pi_rdlock(ProjIndex *pi);
void pi_unlock(ProjIndex *pi);
size_t pi_nrecs(const ProjIndex *pi);            /* under the lock */
const PiRec *pi_rec(const ProjIndex *pi, size_t i);

/* The records that have key k (5.3, 10.1), under the lock: each with the index of
 * k in the record's KEYS, so its DECL, REFS and CALL entries name it.  Sorted by
 * (path, digest); *out is malloc'd. */
typedef struct PiHit {
    const PiRec *rec;
    uint32_t ki;
} PiHit;
size_t pi_find(const ProjIndex *pi, const CIdxKey *k, PiHit **out);

/* Heap bytes the index holds (its arrays, not the mappings), the names it
 * lists, and the records it has. */
typedef struct PiStats {
    size_t bytes, names, recs, tus;
} PiStats;
PiStats pi_stats(ProjIndex *pi);

#endif
