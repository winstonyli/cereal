/* pstate.h - what the project index knows about its units and the files
 * they read (docs/B5_DESIGN.md sections 7 and 8.2): the translation units of
 * the compile database, each with the manifest the store has for it, the
 * current state (size, mtime, hash) of every file any manifest lists, and
 * which units are stale.
 *
 * One thread owns a PState (the scheduler's, or a tool's); nothing here
 * locks. */
#ifndef CEREAL_PSTATE_H
#define CEREAL_PSTATE_H

#include "c/cidxrec.h"
#include "c/cidxstore.h"
#include "lsp.h"

typedef struct PPath {           /* one file's current state (7.2) */
    const char *path;
    uint64_t mtime_ns;           /* the stat data of the last look ... */
    uint64_t taken_ns;           /* ... and when it was taken */
    uint64_t hash;               /* 0: the file does not exist */
    uint32_t size;
    bool known;                  /* hash describes (size, mtime_ns) */
    bool racy;                   /* mtime within 2 s of taken_ns: never trusted */
    bool seen;                   /* stat'ed at all */
} PPath;

typedef struct PFil {            /* a file as a unit saw it */
    PPath *p;
    uint64_t hash, mtime_ns;
    uint32_t size, flags;        /* CFIL_* (0 in a chunk) */
} PFil;

typedef struct PChunk {          /* a system chunk, shared by units (5.1) */
    uint64_t digest;
    uint32_t refs, n;
    PFil *f;
} PChunk;

typedef struct PMiss {
    const char *spelled, *from;
    bool angled;
} PMiss;

typedef struct PMan {            /* a loaded manifest */
    uint64_t tukey, started_ns, written_ns;
    uint32_t status, errors, bytes_read, index_ms;
    PFil *fils;
    uint32_t nfils;
    PChunk **chunks;             /* NULL entries: unreadable */
    uint64_t *sysc;              /* the digests, for the GC and re-reads */
    uint32_t nsysc;
    bool lost;                   /* a chunk or record of it is unreadable: stale */
    PMiss *miss;
    uint32_t nmiss;
    uint64_t *recs;              /* distinct record digests, sorted */
    uint32_t nrecs;
} PMan;

typedef enum {
    PS_FRESH = 0,
    PS_NEW,                      /* no manifest */
    PS_FILE,                     /* a file it read differs */
    PS_MISS,                     /* an include it missed now resolves */
    PS_LOST                      /* a chunk or record of it is unreadable */
} PStale;

typedef struct PSched {          /* the scheduler's (project.c); pstate does not look */
    int cls;                     /* priority class 0..2 (8.2) */
    bool pending;                /* stale and not yet seen as such by the scheduler */
    bool recent;                 /* became stale after the startup load: class 1 */
    bool sticky_sub;             /* indexed in a child only from now on (8.5) */
    bool running;                /* a job for it is in the pool */
    uint8_t fails;               /* consecutive failures (8.6) */
    uint64_t retry_ns;           /* after a failure: not before (monotonic); ~0: never */
} PSched;

typedef struct PTu {
    size_t entry;                /* its compile database entry */
    const char *main;            /* absolute */
    uint64_t flag_hash, tukey;
    PMan *man;                   /* NULL: none usable */
    PStale stale;                /* why it is stale (PS_FRESH: fresh) */
    const char *why;             /* the file or include behind it (static or in the state) */
    uint64_t stale_ns;           /* when it became stale (CLOCK_REALTIME) */
    char **dirs;                 /* search path for MISS entries (lazily): the -iquote
                                  * dirs, then from first_angle the -I ones, then the system ones */
    uint32_t ndirs, first_angle;
    PSched sched;
} PTu;

typedef VEC(PTu *) PTuVec;

typedef struct PState {
    Store *store;
    uint64_t build_id;
    LspConfig *cfg;
    Arena a;                     /* path and include strings */
    PPath **slots;               /* open addressing: path -> state */
    size_t cap, npaths;
    PChunk **chunks;             /* digest -> chunk, likewise */
    size_t chunk_cap, nchunks;
    PTuVec tus;
    size_t corrupt;              /* files rejected and removed so far */
    VEC(char *) log;             /* what was rejected, for the caller to report */
} PState;

void pstate_init(PState *ps, Store *store, uint64_t build_id);
void pstate_free(PState *ps);

/* Makes the units of cfg's entries the project's: those passing `pass` (NULL:
 * all) that are C, deduplicated by (file, flag hash).  A unit that is
 * already there stays; a new one gets the store's manifest if it has one; one
 * that went away is dropped (and, for the writer, its manifest removed).
 * Then the file states are settled and every unit evaluated. */
void pstate_set_config(PState *ps, LspConfig *cfg,
                       bool (*pass)(const CompileEntry *, void *), void *ctx);

/* Looks at the files again (7.2): path NULL: every one; else that one.
 * Returns how many had a different hash.  pstate_eval then re-decides which
 * units are stale and returns how many became stale. */
size_t pstate_sweep(PState *ps, const char *path);
size_t pstate_eval(PState *ps);

/* (Re)loads tu's manifest from the store, e.g. after a job wrote it, then
 * evaluates tu.  False: there is none usable. */
bool pstate_reload(PState *ps, PTu *tu);
/* Forgets tu's manifest and deletes it and (chunks) what it listed is left to
 * the GC. */
void pstate_drop_manifest(PState *ps, PTu *tu, bool remove_file);
/* A record file the manifests name could not be read: removes it and makes
 * the units that list it stale. */
void pstate_reject_rec(PState *ps, uint64_t digest);

/* The digests in use, sorted (6.4); free the three arrays. */
void pstate_live(PState *ps, StoreLive *live);
/* Maps every record the manifests name, rejecting (pstate_reject_rec) those
 * that are missing or unreadable.  Costs reading the cache. */
void pstate_check_records(PState *ps);

PPath *pstate_path(PState *ps, const char *path);   /* NULL: unknown */
const char *pstate_why(PStale s);

#endif
