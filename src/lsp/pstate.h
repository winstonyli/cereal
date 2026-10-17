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
    uint32_t first_dir, how;     /* CRecMiss: where the search began; CMISS_* */
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
    uint64_t ino, mtime_ns;      /* of the file it was read from (pstate_refresh) */
} PMan;

typedef enum {
    PS_FRESH = 0,
    PS_NEW,                      /* no manifest */
    PS_FILE,                     /* a file it read differs */
    PS_MISS,                     /* an include it missed now resolves */
    PS_LOST,                     /* a chunk or record of it is unreadable */
    PS_COVER                     /* fresh, but virtual units are on and it has no overlay sidecar */
} PStale;

typedef struct PSched {          /* the scheduler's (project.c); pstate does not look */
    int cls;                     /* priority class 0..2 (8.2) */
    bool pending;                /* stale and not yet seen as such by the scheduler */
    bool recent;                 /* became stale after the startup load: class 1 */
    struct IJob *job;            /* the job in the pool for it (finish clears only its own) */
    struct {                     /* how and when the next attempt runs (8.5, 8.6) */
        bool sub;                /* in a child only from now on */
        uint8_t fails;           /* consecutive failures */
        uint64_t at;             /* after a failure: not before (monotonic); ~0: never */
    } next;
} PSched;

typedef struct PVc {             /* an overlay of a unit's sidecar, and the unit it makes */
    char *line;
    uint64_t flag_hash, tukey;
} PVc;

typedef struct PTu {
    size_t entry;                /* its compile database entry */
    const char *main;            /* absolute */
    uint64_t flag_hash, tukey;
    const char *overlay;         /* a virtual unit: the words after the entry's flags (in the
                                  * state); NULL: the entry itself */
    uint64_t parent;             /* a virtual unit: the tukey of the unit whose sidecar names it */
    bool cover_known, covered;   /* the sidecar's existence, as of the last (re)load */
    PVc *vc;                     /* its sidecar's overlays (pstate_allocate) */
    uint32_t nvc;
    bool vc_known;
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
    double vratio;               /* virtual units: at most this many per real unit ... */
    size_t vmax;                 /* ... and this many per unit; 0: none (pstate_set_virtual) */
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

/* Virtual units (docs/CONFIG_DESIGN.md 3.7, 6): for each real unit, the overlays
 * its sidecar names (`cereal index-summary --cover`), up to `per_unit` of them
 * and `ratio` times the number of real units in all, become units of their own
 * (the entry's flags plus the overlay).  Set before pstate_set_config; with
 * ratio 0 (the default) a real unit needs no sidecar. */
void pstate_set_virtual(PState *ps, double ratio, size_t per_unit);
/* Makes the virtual units the sidecars name the project's (and drops the others):
 * the budget goes to the real units in main-path order.  Unless force, only when
 * every real unit is fresh (the caller says when the rest cannot become so).
 * pstate_set_config does it for fresh units.  *added, *dropped: how many units. */
void pstate_allocate(PState *ps, bool force, size_t *added, size_t *dropped);
/* Whether a manifest status marks a virtual unit unfit (PARTIAL, FAILED, OVER_LIMIT). */
bool pstate_bad_status(int status);

/* Looks at the files again (7.2): path NULL: every one; else that one.
 * Returns how many had a different hash.  pstate_eval then re-decides which
 * units are stale and returns how many became stale. */
size_t pstate_sweep(PState *ps, const char *path);
size_t pstate_eval(PState *ps);

/* (Re)loads tu's manifest from the store, e.g. after a job wrote it, then
 * evaluates tu.  False: there is none usable. */
bool pstate_reload(PState *ps, PTu *tu);
/* The same if the file is not the one loaded (another inode or mtime); false:
 * unchanged, nothing was done.  A reader follows the writer's GEN with it. */
bool pstate_refresh(PState *ps, PTu *tu);
/* A record file the manifests name is missing or unreadable (why): it is
 * reported (ps->log), removed if this process is the writer, and the units
 * that list it become PS_LOST.  The one corruption path: pi_set_tu calls it
 * when it loads the records (6.3). */
void pstate_reject_rec(PState *ps, uint64_t digest, const char *why);

/* The digests in use, sorted (6.4); free the three arrays. */
void pstate_live(PState *ps, StoreLive *live);

PPath *pstate_path(PState *ps, const char *path);   /* NULL: unknown */
const char *pstate_why(PStale s);

#endif
