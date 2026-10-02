/* intern.h - concurrent identifier interner.
 *
 * Lookups are lock-free: each of the 64 shards publishes an immutable-slot
 * open-addressing table through an atomic pointer, and slots are only ever
 * written once (NULL -> Ident).  Insertions take the shard lock; resizing
 * publishes a new table and retires the old one (kept until the interner is
 * freed, so concurrent readers never see freed memory).  Ids are dense and
 * resolved through a two-level page table with stable addresses.
 *
 * Idents are immutable once published and carry no build state (macro
 * state lives in a MacroTab), so an interner can be shared by successive
 * builds. */
#ifndef CEREAL_INTERN_H
#define CEREAL_INTERN_H

#include "common.h"
#include "thread.h"

typedef struct Ident {
    const char *str;
    uint32_t len;
    uint32_t hash;
    uint32_t id;           /* dense, >= 1 */
    uint64_t digest;       /* 64-bit content hash (hash.h: token hashes) */
    uint16_t kw;           /* keyword/special id, 0 if none */
    uint16_t ckw;          /* C keyword (c/ckw.h), 0 if none */
    uint8_t ext;           /* spelled with a backslash or a byte >= 0x80 */
} Ident;

#define INTERN_SHARDS 64
#define ID_PAGE_BITS 12
#define ID_PAGE (1u << ID_PAGE_BITS)
#define ID_PAGES 16384u   /* up to 64M identifiers */

typedef struct InternTable {
    size_t cap;            /* power of two */
    Ident *slot[1];        /* cap entries (allocated with the table) */
} InternTable;

typedef struct InternShard {
    Mutex m;
    InternTable *table;    /* atomic */
    size_t count;
    Arena arena;
    VEC(InternTable *) retired;
} InternShard;

typedef struct Interner {
    InternShard shards[INTERN_SHARDS];
    Ident **pages[ID_PAGES];   /* atomic page pointers */
    uint32_t next_id;          /* atomic */
    Mutex page_lock;
    uint32_t refs;             /* atomic; interner_new/retain/release */
} Interner;

void interner_init(Interner *in);
void interner_free(Interner *in);
/* A heap interner that successive builds can share (ids stay stable). */
Interner *interner_new(void);
Interner *interner_retain(Interner *in);
void interner_release(Interner *in);
Ident *intern(Interner *in, const char *s, size_t n);
Ident *intern_cstr(Interner *in, const char *s);
/* Lookup only: NULL if s was never interned. */
Ident *intern_find(const Interner *in, const char *s, size_t n);
uint32_t interner_count(const Interner *in);   /* highest id + 1 */

static inline Ident *ident_by_id(const Interner *in, uint32_t id)
{
    return in->pages[id >> ID_PAGE_BITS][id & (ID_PAGE - 1)];
}

/* Iterate all identifiers (not concurrently with interning). */
#define INTERNER_FOREACH(in, b, id)                                       \
    for (uint32_t b = 1; b < interner_count(in); b++)                     \
        for (Ident *id = ident_by_id((in), b); id; id = NULL)

#endif
