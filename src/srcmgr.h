/* srcmgr.h - source files and the global location space. */
#ifndef CEREAL_SRCMGR_H
#define CEREAL_SRCMGR_H

#include "common.h"

/* A location in the global offset space; 0 is invalid. */
typedef uint32_t SrcLoc;

typedef struct SrcRange {
    SrcLoc begin, end; /* half-open */
} SrcRange;

typedef enum {
    SF_USER,
    SF_SYSTEM,
    SF_VIRTUAL /* <built-in>, <command line>, _Pragma scratch */
} SrcFileKind;

typedef struct SrcFile {
    int id;
    const char *path;      /* as opened (normalized) */
    const char *name;      /* presumed name for diagnostics/__FILE__ */
    const char *buf;       /* contents, NUL terminated */
    uint32_t size;
    SrcLoc base;           /* location of buf[0] */
    uint32_t *lines;       /* offsets of line starts */
    uint32_t nlines;
    SrcFileKind kind;
    bool pragma_once;
    bool system_header;    /* set by #pragma GCC system_header or dir */
    struct Ident *guard;   /* detected include guard macro */
    bool guard_checked;
    void *skel;            /* cached Skeleton (skel.c) */
    void *user;            /* analyzer scratch */
} SrcFile;

typedef struct SrcMgr {
    Arena *arena;
    VEC(SrcFile *) files;
    SrcLoc next_base;
} SrcMgr;

void srcmgr_init(SrcMgr *sm, Arena *a);
void srcmgr_free(SrcMgr *sm);

/* Load a file from disk (cached by path). Returns NULL if unreadable. */
SrcFile *srcmgr_load(SrcMgr *sm, const char *path, SrcFileKind kind);
/* Create an in-memory buffer (contents copied). */
SrcFile *srcmgr_add_virtual(SrcMgr *sm, const char *name, const char *buf,
                            size_t len);

SrcFile *srcmgr_file_of(const SrcMgr *sm, SrcLoc loc);
uint32_t srcmgr_offset(const SrcFile *f, SrcLoc loc);
/* 1-based line and column (column counts bytes). */
void srcmgr_linecol(const SrcFile *f, SrcLoc loc, uint32_t *line, uint32_t *col);
SrcLoc srcmgr_loc_of(const SrcFile *f, uint32_t line, uint32_t col);
/* Pointer to the line containing loc and its length (without newline). */
const char *srcmgr_line_text(const SrcFile *f, uint32_t line, uint32_t *len);

char *path_dirname(Arena *a, const char *path);
char *path_join(Arena *a, const char *dir, const char *file);
char *path_normalize(Arena *a, const char *path);

#endif
