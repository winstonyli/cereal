/* srcmgr.h - source files and the global location space.
 *
 * Every byte cereal ever lexes lives in one reserved virtual-address region:
 * a SrcLoc is an offset into it, so the text at a location is simply
 * `region + loc`.  Each file (or scratch chunk) starts on a page boundary
 * and is followed by at least SRC_PAD zero bytes, so scanners may read
 * past the end of any buffer without bounds checks. */
#ifndef CEREAL_SRCMGR_H
#define CEREAL_SRCMGR_H

#include "common.h"
#include "thread.h"

typedef uint32_t SrcLoc;   /* 0 is invalid */

typedef struct SrcRange {
    SrcLoc begin, end;     /* half-open */
} SrcRange;

#define SRC_PAD 64

typedef enum {
    SF_USER,
    SF_SYSTEM,
    SF_VIRTUAL,            /* <built-in>, <command line> */
    SF_SCRATCH             /* synthesized spellings (##, #, builtins) */
} SrcFileKind;

typedef struct SrcFile {
    int id;
    const char *path;      /* as opened (normalized) */
    const char *name;      /* presumed name for diagnostics/__FILE__ */
    const char *buf;       /* == region + base; NUL + zero padding after */
    uint32_t size;         /* bytes of content */
    uint32_t span;         /* location range reserved for this file */
    SrcLoc base;
    uint32_t *lines;       /* lazily computed line starts (atomic publish) */
    uint32_t nlines;
    uint32_t cr_state;     /* atomic: 0 unknown, 1 no '\r', 2 has '\r' */
    SrcFileKind kind;
    bool pragma_once;
    bool system_header;
    uint32_t *sysmarks;    /* line markers: (physical line, 1 = system flag) pairs */
    uint32_t nsysmarks;    /* entries in sysmarks (2 per marker) */
    struct Ident *guard;   /* detected include guard macro */
    bool guard_checked;
    void *skel;            /* cached Skeleton (skel.c) */
    void *user;            /* analyzer scratch */
} SrcFile;

#define FILE_CHUNK 1024u
#define FILE_CHUNKS 4096u

/* A thread's current scratch chunk (each worker owns one). */
typedef struct ScratchCursor {
    struct SrcFile *chunk;
} ScratchCursor;

/* Thread safety: loading, reserving and registering take `m`.  The file
 * registry is append-only with atomic publication, so srcmgr_file_of and
 * srcmgr_file never lock.  Scratch writes go through a per-thread cursor. */
typedef struct SrcMgr {
    Arena *arena;          /* guarded by m */
    Mutex m;
    char *region;
    size_t region_size;
    size_t committed;      /* bytes of region made readable */
    SrcLoc next_base;
    SrcFile **fchunks[FILE_CHUNKS];
    uint32_t nfiles;       /* atomic */
    SrcFile **path_slots;  /* open-addressing map path -> file */
    size_t path_cap, path_count;
    ScratchCursor scratch; /* the main thread's cursor */
    /* Editor buffers: consulted before the file system (normalized path);
     * returns false to fall through to the disk. */
    bool (*overlay)(void *ctx, const char *path, const char **buf,
                    size_t *len);
    void *overlay_ctx;
} SrcMgr;

static inline uint32_t srcmgr_nfiles(const SrcMgr *sm)
{
    return atomic_load_u32(&sm->nfiles);
}
static inline SrcFile *srcmgr_file(const SrcMgr *sm, uint32_t i)
{
    return sm->fchunks[i / FILE_CHUNK][i % FILE_CHUNK];
}

void srcmgr_init(SrcMgr *sm, Arena *a);
void srcmgr_free(SrcMgr *sm);

/* Load a file (cached by normalized path).  NULL if unreadable. */
SrcFile *srcmgr_load(SrcMgr *sm, const char *path, SrcFileKind kind);
/* In-memory buffer (contents copied into the region). */
SrcFile *srcmgr_add_virtual(SrcMgr *sm, const char *name, const char *buf,
                            size_t len);
/* Copy bytes into the scratch area through a thread's cursor; returns
 * their location.  The bytes are followed by a NUL. */
SrcLoc srcmgr_scratch(SrcMgr *sm, ScratchCursor *c, const char *s, size_t n);

/* Scratch written through a cursor after a mark can be taken back (when
 * nothing refers to it any more: token regeneration, toks.h). */
typedef struct ScratchMark {
    struct SrcFile *chunk;
    uint32_t size;
} ScratchMark;
ScratchMark srcmgr_scratch_mark(const ScratchCursor *c);
void srcmgr_scratch_rewind(ScratchCursor *c, ScratchMark m);

static inline const char *srcmgr_ptr(const SrcMgr *sm, SrcLoc loc)
{
    return sm->region + loc;
}

SrcFile *srcmgr_file_of(const SrcMgr *sm, SrcLoc loc);
uint32_t srcmgr_offset(const SrcFile *f, SrcLoc loc);
/* 1-based line and column (column counts bytes). */
void srcmgr_linecol(SrcFile *f, SrcLoc loc, uint32_t *line, uint32_t *col);
/* A '# N "file" [flags]' marker: from physical line `line` on, the text is in
 * a system header iff `sys` (flag 3). */
void srcmgr_mark_system(SrcFile *f, uint32_t line, bool sys);
/* Is loc in a system header, counting line-marker flags? */
bool srcmgr_is_system(SrcFile *f, SrcLoc loc);
SrcLoc srcmgr_loc_of(SrcFile *f, uint32_t line, uint32_t col);
const char *srcmgr_line_text(SrcFile *f, uint32_t line, uint32_t *len);

/* Incremental line lookup for monotone (mostly forward) queries; owned
 * by one thread.  Remembers positions in a few recent files so that
 * returning from an #include resumes counting instead of restarting. */
#define LINECURSOR_WAYS 8
typedef struct LineCursor {
    SrcFile *file;
    uint32_t off;
    uint32_t line;
    uint32_t next_nl;      /* offset of the first newline at or after off */
    struct {
        SrcFile *file;
        uint32_t off, line;
    } saved[LINECURSOR_WAYS];
    unsigned next_way;
} LineCursor;
uint32_t linecursor_slow(LineCursor *c, SrcFile *f, SrcLoc loc);
static inline uint32_t linecursor_line(LineCursor *c, SrcFile *f, SrcLoc loc)
{
    uint32_t off = loc - f->base;
    if (c->file == f && off >= c->off && off <= c->next_nl)
        return c->line;
    return linecursor_slow(c, f, loc);
}

char *path_dirname(Arena *a, const char *path);
char *path_join(Arena *a, const char *dir, const char *file);
char *path_normalize(Arena *a, const char *path);

/* #line adjustments of one file entry, newest first; immutable and
 * shared.  A location resolves through the adjustment in effect where it
 * was spelled (GCC), not the latest one: a #line inside an argument list
 * does not move the tokens read before it. */
typedef struct LineAdj {
    const struct LineAdj *prev;
    uint32_t from;             /* first physical line affected */
    int32_t delta;             /* presumed = physical + delta */
} LineAdj;

static inline int32_t line_adj_delta(const LineAdj *a, uint32_t phys)
{
    for (; a; a = a->prev)
        if (phys >= a->from)
            return a->delta;
    return 0;
}

#endif
