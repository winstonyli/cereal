/* cidxstore.h - the index cache on disk (docs/B5_DESIGN.md section 6):
 * where it lives, who may write it, what is kept.  The files themselves
 * (manifests, records, system chunks) are cidxrec.h's. */
#ifndef CEREAL_CIDXSTORE_H
#define CEREAL_CIDXSTORE_H

#include "common.h"

/* One project's directory, `<cache>/index/<project id>/` (5.1). */
typedef struct Store {
    char *cache;         /* the cache root */
    char *dir;           /* the project directory */
    int lock_fd, gen_fd; /* LOCK and GEN, open for the store's life */
    int use_fd;          /* USE, held shared: an evictor skips a directory in use (6.4) */
    bool writer;         /* this process holds the flock on LOCK (6.5) */
    bool private_root;   /* a mkdtemp directory, removed by store_close (6.1) */
} Store;

/* The project id of a workspace root, compile database path and build, and its
 * directory under a cache root (malloc'd; nothing is created). */
uint64_t store_project_id(const char *root, const char *db, uint64_t build_id);
char *store_project_dir(const char *cache, const char *root, const char *db,
                        uint64_t build_id);

/* Opens (creating) the project's directory.  The cache root is cache_dir,
 * else $XDG_CACHE_HOME/cereal, else $HOME/.cache/cereal; cache_dir "" asks for
 * a private temporary root, which is also the fallback when the root is not
 * writable or its files cannot be opened: warn then says why (it stays empty for
 * "").  The private root is named for this process; those of dead ones are
 * removed. */
bool store_open(Store *s, const char *cache_dir, const char *root, const char *db,
                uint64_t build_id, char *warn, size_t warnn);
void store_close(Store *s);

/* Takes the project's writer lock if free (non-blocking); true if this
 * process holds it now. */
bool store_lock(Store *s);
/* The publish counter in GEN (readers poll it), and the writer's increment. */
uint64_t store_gen(Store *s);
void store_gen_bump(Store *s);

/* The digests and keys (sorted) of what is in use (6.4). */
typedef struct StoreLive {
    const uint64_t *tus, *recs, *chunks;
    size_t ntus, nrecs, nchunks;
} StoreLive;
typedef struct StoreGc {
    size_t tus, recs, chunks;    /* files removed */
} StoreGc;
/* Removes the manifests, records and chunks not in live, once they are older
 * than 10 minutes (a job may not have written its manifest yet; a compdb being
 * rewritten may omit units for a while), and stray temporary files likewise. */
void store_gc(Store *s, const StoreLive *live, StoreGc *out);

/* While `<cache>/index` is over cap_mb, removes whole project directories,
 * least recently used first (by `meta`), never this one and never one a live
 * process has open (its USE lock).  Returns the number removed. */
size_t store_evict(Store *s, double cap_mb);

#endif
