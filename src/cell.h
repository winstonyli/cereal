/* cell.h - the cell cache: phase-B results that survive edits.
 *
 * A cell is a run of plan items [s, e) that a phase-B worker preprocessed
 * starting fresh at s and left clean at e (no invocation in flight), so
 * the run's results depend only on what it read.  A cell stores those
 * results *relative to itself*:
 *   - locations as (item of the cell, offset), (definition it read,
 *     offset) or (ancestor frame of an item: an #include line outside);
 *   - definitions as indexes into its read set;
 *   - versions as the item that set them.
 * Nothing in a cell points into the build that made it (identifiers
 * excepted: builds of a unit share an interner), so a later build can
 * reuse it wherever the same items reappear and the same reads give the
 * same answers, then turn it back into absolute results.
 *
 * Key, checked on reuse:
 *   - the items: kind, segment text, pragma text, frame (file, presumed
 *     name, depth, system flag), split flag;
 *   - the read set: every (identifier, item) looked up or lexed, with the
 *     fingerprint of the definition found (misses and poisoning included);
 *     reads that found one definition must find one definition again;
 *   - absolute line numbers, when __LINE__ was expanded;
 *   - whether the cell ends the TU.
 * Options are checked once per cache (CellCache.config).  See
 * docs/LSP.md. */
#ifndef CEREAL_CELL_H
#define CEREAL_CELL_H

#include "common.h"
#include "diag.h"
#include "plan.h"
#include "pp.h"

typedef uint64_t CLoc;          /* encoded location; 0: none */

/* kind (2 bits) | a (30 bits) | offset (32 bits): CLocs of one kind and
 * anchor sort by offset. */
enum { CL_NONE, CL_ITEM, CL_MACRO, CL_FRAME };
#define CLOC(k, a, off) (((uint64_t)(k) << 62) | ((uint64_t)(a) << 32) | (off))
#define CLOC_KIND(l) ((unsigned)((l) >> 62))
#define CLOC_A(l) ((uint32_t)(((l) >> 32) & 0x3FFFFFFFu))
#define CLOC_OFF(l) ((uint32_t)(l))
#define CLOC_AMAX 0x3FFFFFFFu
#define CELL_ALIAS_BIT 0x80000000u

/* An encoded definition, given what the cell's reads found. */
static inline struct Macro *cell_macro(struct Macro *const *rmacro,
                                       uint32_t m)
{
    struct Macro *x;
    if (m == UINT32_MAX)
        return NULL;
    x = rmacro[m & ~CELL_ALIAS_BIT];
    return (m & CELL_ALIAS_BIT) ? x->alias_of : x;
}

/* ---- recording reads (phase-B workers) ------------------------------- */

#define CELL_RECENT 1024

typedef struct CellReads {
    uint64_t recent[CELL_RECENT]; /* direct-mapped: reads just noted */
    uint64_t *slot;             /* open addressing; 0 = empty */
    size_t cap, n;
    VEC(uint64_t) list;         /* (item << 32 | ident), insertion order */
    VEC(uint32_t) line_items;   /* keys where __LINE__ was expanded */
} CellReads;

void cell_reads_init(CellReads *r);
void cell_reads_free(CellReads *r);
/* After the run: by item, as cell_enc_begin needs. */
void cell_reads_sort(CellReads *r);

/* ---- cells -------------------------------------------------------------- */

typedef struct CellRead {
    struct Ident *name;
    uint32_t item;              /* relative item whose version applies */
    uint32_t cls;               /* first read that found the same definition;
                                   UINT32_MAX: found none */
    uint64_t val;               /* fingerprint of what was found */
} CellRead;

typedef struct CellNote {
    CLoc loc;
    const char *msg;
} CellNote;

typedef struct CellDiag {
    uint8_t level, once;
    const char *id;             /* static option name */
    uint32_t key;               /* relative item */
    CLoc loc, range_b, range_e;
    const char *msg, *fixit;
    CellNote *notes;
    uint32_t nnotes;
    CLoc *inc;
    uint32_t ninc;
} CellDiag;

/* Expansions per definition (for Macro.expansions). */
typedef struct CellExp {
    uint32_t macro;             /* encoded definition */
    uint32_t count;
} CellExp;

#define CELL_MAX_CLIENTS 4

typedef struct Cell {
    Arena arena;                /* everything below */
    uint32_t nitems;
    uint64_t *item_hash;
    bool at_end;                /* e was the end of the TU */
    uint32_t *lines;            /* __LINE__ used: presumed line of each item
                                   begin (NULL otherwise) */
    CellRead *reads;
    uint32_t nreads;
    CellDiag *diags;
    uint32_t ndiags;
    CellExp *exps;
    uint32_t nexps;
    uint32_t ntoks;             /* tokens it gives, and their sequence */
    uint64_t tok_hash;          /* hash (toks.h) */
    void *blob[CELL_MAX_CLIENTS];
    uint64_t gen;               /* last build that used it */
    uint32_t refs;              /* atomic: the cache's and indexes' */
    struct Cell *next;          /* CellCache bucket chain */
} Cell;

typedef struct CellStats {
    size_t cells, hits, misses, invalidated, stored, uncacheable;
    size_t hit_items, miss_items;
} CellStats;

typedef struct CellCache {
    uint64_t config;            /* what the cells were made with */
    int nclients;
    Cell **bucket;              /* by item_hash[0] */
    size_t nbuckets, n;
    uint64_t gen;               /* builds so far */
    CellStats last;             /* of the last build */
} CellCache;

void cell_cache_init(CellCache *c);
void cell_cache_free(CellCache *c);
/* Cells are reference counted: an index built from a cell keeps it after
 * the cache has dropped it.  A new cell has one reference. */
Cell *cell_retain(Cell *c);
void cell_release(Cell *c);
/* A build: cells it neither reuses nor stores are dropped at the end
 * (all of them if the configuration changed). */
void cell_cache_begin(CellCache *c, uint64_t config, int nclients);
void cell_cache_end(CellCache *c);


/* ---- per-build context ---------------------------------------------------- */

/* What a build knows for keys and validation: item hashes, definition
 * fingerprints (by Macro.id). */
typedef struct CellBuild {
    PP *pp;                     /* after phase A */
    Plan *plan;
    uint64_t *item_hash;
    uint64_t *fp;               /* by Macro.id */
    size_t nfp;
    uint32_t *stamp;            /* validation scratch by Macro.id */
    uint32_t gen;
    struct CellFrames *frames;  /* frame digests, by PlanFrame */
} CellBuild;

void cell_build_init(CellBuild *b, PP *pp, Plan *plan);
void cell_build_free(CellBuild *b);
/* Is item i a cell boundary candidate? */
bool cell_candidate(const Plan *plan, size_t i);

/* A cached cell valid at item s of this build: definitions its reads
 * resolve to now (out, nreads entries) are filled in. */
Cell *cell_lookup(CellCache *c, CellBuild *b, size_t s, Macro ***out);

/* ---- encoding (by the build that ran the cell) ---------------------------- */

typedef struct CellEnc {
    CellBuild *b;
    Cell *cell;
    size_t s, e;
    bool ok;                    /* false: something could not be encoded */
    Macro **rmacro;             /* per read: what it found (this build) */
    /* locations: item ranges and definitions, sorted by start */
    struct CellSpan *spans;
    size_t nspans;
    struct CellMacIdx *macs;
    size_t nmacs;
    VEC(CellDiag) diags;        /* copied to the cell at the end */
    VEC(CellExp) exps;
    uint32_t *exp_slot;         /* open addressing by macro: exps index + 1 */
    uint32_t exp_cap;
} CellEnc;

/* Start a cell [s, e) from a worker's reads; NULL if it has none of its
 * own to give (e.g. empty). */
void cell_enc_begin(CellEnc *e, CellBuild *b, const CellReads *r, size_t s,
                    size_t end);
/* The finished cell, or NULL (and the cell freed) if encoding failed.
 * rmacro_out (may be NULL): what the cell's reads found in this build, for
 * using the cell right away (malloc'd; the caller frees it). */
Cell *cell_enc_end(CellEnc *e, Macro ***rmacro_out);
Arena *cenc_arena(CellEnc *e);
CLoc cenc_loc(CellEnc *e, SrcLoc loc);
uint32_t cenc_macro(CellEnc *e, const Macro *m);   /* UINT32_MAX: NULL */
uint32_t cenc_item(CellEnc *e, uint32_t item);
const char *cenc_str(CellEnc *e, const char *s);
void cenc_diag(CellEnc *e, const Diagnostic *d);
void cenc_exp(CellEnc *e, uint32_t key, const Macro *m);

/* ---- decoding (by a build reusing the cell) ---------------------------- */

typedef struct CellDec {
    CellBuild *b;
    const Cell *cell;
    size_t s;
    Macro **rmacro;             /* per read, in this build */
    Arena *arena;               /* where decoded results live */
} CellDec;

SrcLoc cdec_loc(const CellDec *d, CLoc l);
Macro *cdec_macro(const CellDec *d, uint32_t m);
uint32_t cdec_item(const CellDec *d, uint32_t rel);
uint32_t cdec_version(const CellDec *d, uint32_t rel);
Diagnostic *cdec_diag(const CellDec *d, const CellDiag *cd);

/* Store a cell (the cache takes the caller's reference). */
void cell_cache_put(CellCache *c, Cell *cell);

#endif
