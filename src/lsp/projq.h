/* projq.h - answering from the project index (docs/B5_DESIGN.md 10.3, 10.4, 11).
 *
 * One query holds a PqFiles: the files its answers lie in, each with the text the
 * client has (the snapshot's, an open document's, else the disk's), its line
 * table, and the mapping between that text and the disk text the project's records
 * were built from.  Everything a request finds, from its open unit or from the
 * project, is placed through it and written by it, so there is one position
 * mechanism and one dedupe (path, offset, length).  The workspace/symbol,
 * references, definition, declaration and call hierarchy features share it. */
#ifndef CEREAL_PROJQ_H
#define CEREAL_PROJQ_H

#include "projidx.h"

typedef struct PqFile {
    char *path;
    const char *uri;             /* of path (the files' arena) */
    const char *cur;             /* the text the client has: loaded at first use */
    size_t curlen;
    char *own;                   /* cur's buffer if it was read (else it is borrowed) */
    uint64_t curhash;
    bool open;                   /* cur may differ from the disk's: a snapshot's or
                                  * an open document's text */
    bool loaded, hashed, disk_loaded, fwd_done, rev_done, lines_built;
    char *disk;                  /* open: the disk's text, when it is needed */
    size_t disklen;
    uint64_t diskhash;
    CIdxEdit fwd, rev;           /* disk to cur, and back */
    LineTable lines;             /* of cur */
} PqFile;

typedef struct PqFiles {
    VEC(PqFile *) v;
    PqFile *last;
    SrcMgr *sm;                  /* the request's snapshot: its files' texts first (or NULL) */
    WsDocText doc_text;          /* then an open document's (or NULL) */
    PosEncoding enc;
    Arena a;                     /* uris and the keys' strings */
    bool hint;                   /* a file changed behind the index's back (7.3) */
} PqFiles;

void pq_init(PqFiles *fs, SrcMgr *sm, WsDocText doc_text, PosEncoding enc);
void pq_free(PqFiles *fs);          /* also asks for a sweep if fs->hint */
PqFile *pq_file(PqFiles *fs, const char *path);

/* Where something the records say about f is in the client's text.  off, len
 * are in the text the record was built from (fhash is its hash); false: that
 * text is gone, or the edit touched it (and the sweep is hinted). */
bool pq_place(PqFiles *fs, PqFile *f, uint64_t fhash, uint32_t off, uint32_t len,
              uint32_t *out);
/* The reverse: the place in the disk's text of what is at off, len in the
 * client's, for keys anchored in it; false: inside the damage. */
bool pq_unplace(PqFiles *fs, PqFile *f, uint32_t off, uint32_t len, uint32_t *out);

/* LSP positions, ranges and locations in the client's text of f. */
void pq_pos(JsonWriter *w, PqFiles *fs, PqFile *f, size_t off);
void pq_range(JsonWriter *w, PqFiles *fs, PqFile *f, size_t off, size_t len);
void pq_location(JsonWriter *w, PqFiles *fs, PqFile *f, size_t off, size_t len);

/* ---- locations, deduplicated ------------------------------------------------ */

typedef struct PqLoc {
    PqFile *f;
    uint32_t off, len;           /* placed in the client's text */
    uint8_t seq;                 /* 0: an open unit's, 1: the project's */
} PqLoc;
typedef VEC(PqLoc) PqLocs;

/* Sorted by (path, offset, length), a repeated one dropped (an open unit's wins
 * over the project's). */
void pq_locs_sort(PqLocs *v);
/* The array body: one Location each, in order (call pq_locs_sort first). */
void pq_locs_write(JsonWriter *w, PqFiles *fs, const PqLocs *v);

/* ---- keys ------------------------------------------------------------------- */

/* The key of the open unit's decl d (cindex_key), its strings in fs's arena and
 * its anchor in the disk's text, which is what the records have.  False: the
 * anchor is in an edited stretch, so the project has nothing under it (the key
 * is still usable for the open unit). */
bool pq_key(PqFiles *fs, const CIndex *ix, uint32_t d, CIdxKey *out);
/* A key as hex text (item data) and back; false: not one. */
const char *pq_key_hex(PqFiles *fs, const CIdxKey *k);
bool pq_key_parse(PqFiles *fs, const char *hex, CIdxKey *out);
int pq_key_cmp(const CIdxKey *a, const CIdxKey *b);

/* ---- what the records say about a key --------------------------------------- */

enum { PQ_REF = 1, PQ_DECL = 2, PQ_DEF = 4 };
/* What a rename needs to know about the entries it does not list (R3, R4, R7). */
typedef struct PqBlock {
    const char *why;             /* the first use that cannot be renamed
                                  * (cindex_rename_blocker), at f, off */
    PqFile *f;                   /* where, for why and for the first unplaced entry */
    uint32_t off;
    unsigned ndropped;           /* entries whose text is gone or edited ... */
    PqFile *df;                  /* ... the first, in the record's text */
    uint32_t doff;
} PqBlock;
/* Appends the project's entries for k: the uses (PQ_REF; not those spelled in a
 * #define body) and the declarations or definitions (the others), placed in
 * the client's text.  Takes the index's read lock. */
void pq_entries(PqFiles *fs, ProjIndex *pi, const CIdxKey *k, unsigned what, PqLocs *out,
                PqBlock *blk);
/* R1: some record has an entry of another key at a place the key has one, in
 * the same text of the file (the same header under other macros, 12.1).  The
 * place goes to blk->f, off. */
bool pq_ambiguous(PqFiles *fs, ProjIndex *pi, const CIdxKey *k, PqBlock *blk);
/* Whether f's text is still the one the answers were made from (a rename's
 * last check): the live document's, else the disk's.  *version: the open
 * document's (-1: none given), -2: it is not open. */
bool pq_current(PqFiles *fs, PqFile *f, long long *version);
/* "path:line:column" of off in the client's text of f. */
void pq_where(PqFiles *fs, PqFile *f, uint32_t off, char *buf, size_t bufsz);

/* A function or variable as a call hierarchy item: where it is declared (its
 * definition if the project has one). */
typedef struct PqSym {
    CIdxKey key;                 /* strings in fs's arena */
    PqFile *f;                   /* NULL: no place is known */
    uint32_t off, len;           /* the name */
    uint32_t ext_b, ext_e;       /* the whole declaration */
    int64_t d;                   /* the open unit's decl (and ev its event), else -1 */
    uint32_t ev;
    bool def;
} PqSym;
typedef struct PqCall {
    PqSym other;                 /* the caller (incoming) or the callee (outgoing) */
    PqLoc site;
} PqCall;
typedef VEC(PqCall) PqCalls;

/* Appends the project's calls to (incoming) or from key k; other.f is NULL where
 * the project has no declaration at hand (pq_sym fills it). */
void pq_calls(PqFiles *fs, ProjIndex *pi, const CIdxKey *k, bool incoming, PqCalls *out);
/* Fills sym.f and its places from the project's declarations of sym.key. */
void pq_sym(PqFiles *fs, ProjIndex *pi, PqSym *sym);

#endif
