/* idxset.h - open-addressing sets of indices into the caller's own storage.
 *
 * A slot holds index + 1 (0 is empty).  The set stores no keys: the caller
 * hashes its key, and gives an equality test against the entry an index
 * names and a hash of that entry (for growing).  Single-threaded; the
 * caller locks.  StrPool is the common case: deduplicated NUL-terminated
 * strings in one buffer, named by byte offset. */
#ifndef CEREAL_IDXSET_H
#define CEREAL_IDXSET_H

#include "common.h"

typedef struct IdxSet {
    uint32_t *slot;      /* cap entries, a power of two (or none yet) */
    size_t cap, n;
} IdxSet;

typedef bool (*IdxEq)(void *ctx, uint32_t idx);
typedef uint64_t (*IdxHash)(void *ctx, uint32_t idx);

/* The slot holding the entry eq accepts on h's probe chain, else the empty
 * slot that ends it (write idx + 1 there with idxset_set, after
 * idxset_reserve).  Valid until the next reserve. */
uint32_t *idxset_probe(const IdxSet *s, uint64_t h, IdxEq eq, void *ctx);
/* Room for n entries (load <= 1/2); existing entries are rehashed by hash. */
void idxset_reserve(IdxSet *s, size_t n, IdxHash hash, void *ctx);
static inline void idxset_set(IdxSet *s, uint32_t *p, uint32_t idx)
{
    s->n += !*p;
    *p = idx + 1;
}
void idxset_free(IdxSet *s);

/* Deduplicated strings; byte 0 is "" (so offset 0 means the empty string). */
typedef struct StrPool {
    StrBuf b;
    IdxSet set;
} StrPool;

/* The offset of s[0..n) (without NULs) in p->b, added if new. */
uint32_t strpool_add(StrPool *p, const char *s, size_t n);
void strpool_free(StrPool *p);

int idxset_selftest(void);   /* `cereal selftest idxset` */

#endif
