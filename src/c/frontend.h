/* frontend.h - parse and check one translation unit.  Shared by
 * `cereal parse|check|-fsyntax-only` (main.c) and the language server's
 * second build phase (lsp/server.c). */
#ifndef CEREAL_FRONTEND_H
#define CEREAL_FRONTEND_H

#include "c/target.h"
#include "driver.h"
#include "par.h"

typedef struct FrontendOpts {
    bool check;                 /* run the checker (else parse only) */
    bool dump;                  /* print the syntax trees to out */
    bool dump_types, dump_summaries, keep_summaries;
    const char *validate_summaries;
    const Target *target;       /* NULL: the host's */
    FILE *out;                  /* the dumps */
    /* Non-NULL: take the parser's tokens from a cell build made with
     * these options (--cells), as the language server has them. */
    const ParOptions *cells_par;
    /* Non-NULL (with check): build the C symbol index into *cidx (NULL if
     * the run was cancelled); verify: --verify-symbols reports there. */
    struct CIndex **cidx;
    FILE *symidx_verify;
} FrontendOpts;

/* Parses (and checks) `path` into tu->diag.  tu must be freshly
 * tu_init'ed (an overlay or pp.cancel set on it is honoured), and stays
 * the caller's: nothing is printed or freed.  A set pp.cancel flag ends
 * the run early, with partial diagnostics.  False: the file could not be
 * opened. */
bool frontend_run(TU *tu, const char *path, const FrontendOpts *fo);

/* Renaming a C name (crename.c; docs/B2_DESIGN.md, "Phase 4 design
 * addendum"): the unit is checked as it is and with the edits applied, and
 * the rename is accepted only if both runs resolve every name alike and
 * give the same diagnostics.  Shared by `cereal query rename=` and the
 * language server (which runs it on its builder thread: the checker keeps
 * static state, so two checks must not run at once). */
typedef struct CRename {
    Options *o;                 /* the unit's */
    const char *main;           /* its main file */
    bool (*overlay)(void *ctx, const char *path, const char **buf,
                    size_t *len);   /* editor buffers, or NULL */
    void *overlay_ctx;
    const char *path;           /* the name: at offset off of this file */
    uint32_t off;
    const char *name;           /* the new name */
    uint32_t *offs;             /* out: the main file's offsets to edit, in
                                   order (malloc'd) */
    size_t n;
    uint32_t len;               /* out: the old name's length */
} CRename;
/* NULL: q->offs; else why the rename is refused (malloc'd). */
char *c_rename(CRename *q);

#endif
