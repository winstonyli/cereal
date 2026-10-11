/* fixres.h - what one translation unit tells `cereal fix` (fixtu.c; the
 * merge and the gate are in fixmode.c; docs/C_DESIGN.md section 6). */
#ifndef CEREAL_FIXRES_H
#define CEREAL_FIXRES_H

#include "common.h"
#include "diag.h"
#include "jobs.h"

/* A user file the unit read. */
typedef struct FFile {
    const char *path;        /* SrcFile.path: how this unit spells it (the
                                key the overlay is asked for) */
    const char *key;         /* its real path */
    uint64_t hash;           /* of the text as read (BOM dropped) */
    uint32_t size;
} FFile;

typedef struct FDiag {
    uint8_t level;           /* DiagLevel */
    const char *id;          /* "" for a hard error */
    const char *msg;
    int32_t file;            /* index into files, -1: not in a user file */
    const char *hdr;         /* it suggests including this header, or NULL */
    uint32_t off, line, col;
} FDiag;

typedef struct FEdit {
    uint32_t file;           /* index into files */
    uint32_t begin, end;
    const char *text;
} FEdit;

/* The fix-its of one diagnostic. */
typedef struct FGroup {
    const char *id;
    uint8_t level;           /* of its diagnostic */
    uint8_t app;             /* FixApplic: the weakest of its fix-its */
    bool paren;              /* only inserts parentheses */
    uint32_t diag;           /* index into diags */
    const char *hdr;         /* it adds the #include of this header, or NULL */
    uint32_t e0, ne;         /* its edits: edits[e0 .. e0 + ne) */
    int32_t pfile;           /* where the first edit is */
    uint32_t pline, pcol;
} FGroup;

typedef struct FRange {
    uint32_t file, begin, end;
} FRange;

/* An edited file's new text, served to a re-run in place of the disk's. */
typedef struct FOv {
    const char *path;
    const char *buf;
    size_t len;
} FOv;

/* Editor buffers (SrcMgr.overlay): false falls through to the disk. */
typedef bool (*FixBaseFn)(void *ctx, const char *path, const char **buf,
                          size_t *len);

typedef struct FixResult {
    Arena arena;
    const FOv *ov;           /* in: files served from memory */
    size_t nov;
    FixBaseFn base;          /* in: the texts under ov (the language server's
                                buffers); NULL: the disk */
    void *base_ctx;
    bool quiet;              /* in: do not report an unreadable input */
    bool opened;             /* the main file could be read */
    uint32_t nerrors;
    uint64_t digest;         /* ast digest of the checked units */
    uint8_t *enabled;        /* diag_enabled_map; malloc'd */
    VEC(FFile) files;
    VEC(FDiag) diags;        /* in the order reported */
    VEC(FEdit) edits;
    VEC(FGroup) groups;      /* in the order of their diagnostics */
    VEC(FRange) inactive;    /* inactive #if groups of user files */
} FixResult;

/* The TuFn of fix mode: check path (and run the lint analyses) and fill the
 * FixResult in res.  Prints only that the input cannot be read. */
int fix_tu(Options *o, const char *path, FILE *out, FILE *err, void *res);
void fixres_free(FixResult *r);

#endif
