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
    uint32_t *lines;       /* lazily computed line starts */
    uint32_t nlines;
    int8_t has_cr;         /* -1 unknown, 0/1 */
    uint32_t lc_off, lc_line; /* last line-cursor position in this file */
    SrcFileKind kind;
    bool pragma_once;
    bool system_header;
    struct Ident *guard;   /* detected include guard macro */
    bool guard_checked;
    void *skel;            /* cached Skeleton (skel.c) */
    void *user;            /* analyzer scratch */
} SrcFile;

typedef struct SrcMgr {
    Arena *arena;
    char *region;
    size_t region_size;
    size_t committed;      /* bytes of region made readable */
    SrcLoc next_base;
    VEC(SrcFile *) files;
    SrcFile **path_slots;  /* open-addressing map path -> file */
    size_t path_cap, path_count;
    SrcFile *scratch;
} SrcMgr;

void srcmgr_init(SrcMgr *sm, Arena *a);
void srcmgr_free(SrcMgr *sm);

/* Load a file (cached by normalized path).  NULL if unreadable. */
SrcFile *srcmgr_load(SrcMgr *sm, const char *path, SrcFileKind kind);
/* In-memory buffer (contents copied into the region). */
SrcFile *srcmgr_add_virtual(SrcMgr *sm, const char *name, const char *buf,
                            size_t len);
/* Copy bytes into the scratch area; returns their location.  The bytes
 * are followed by a NUL. */
SrcLoc srcmgr_scratch(SrcMgr *sm, const char *s, size_t n);

static inline const char *srcmgr_ptr(const SrcMgr *sm, SrcLoc loc)
{
    return sm->region + loc;
}

SrcFile *srcmgr_file_of(const SrcMgr *sm, SrcLoc loc);
uint32_t srcmgr_offset(const SrcFile *f, SrcLoc loc);
/* 1-based line and column (column counts bytes). */
void srcmgr_linecol(SrcFile *f, SrcLoc loc, uint32_t *line, uint32_t *col);
SrcLoc srcmgr_loc_of(SrcFile *f, uint32_t line, uint32_t col);
const char *srcmgr_line_text(SrcFile *f, uint32_t line, uint32_t *len);

/* Incremental line lookup for monotone (mostly forward) queries. */
typedef struct LineCursor {
    SrcFile *file;
    uint32_t off;
    uint32_t line;
    uint32_t next_nl;      /* offset of the first newline at or after off */
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

#endif
