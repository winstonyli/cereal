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
    bool verify_tast;           /* with dump_tast: check each tree (tast.c) */
    unsigned *tast_bad;         /* with verify_tast: incremented per finding */
    bool dump_types, dump_tast, dump_summaries, keep_summaries;
    bool dump_cfg, verify_cfg;  /* the control-flow graph of each function (tcfg.c) */
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
    uint64_t *digest;           /* non-NULL (with check): mixed with each unit's AST digest */
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
 * language server (which runs it on its builder thread, to keep the checks
 * in order beside the edits; checks may run concurrently, D5 of
 * docs/B5_DESIGN.md). */
typedef struct CRenFile {
    const char *path;
    uint32_t *offs;             /* ascending offsets of the old name in the
                                   file's current text */
    size_t n;
} CRenFile;
/* What a unit's check found of the name in code it did not compile (inactive #if
 * groups of user files, A3.9), and the files its index has.  With `inact` set,
 * c_rename records these instead of refusing: the caller refuses what no unit
 * in another configuration compiles (c_rename_uncovered). */
typedef struct CRenHit {
    char *path;                 /* the file */
    uint32_t off;               /* the word's offset in it */
    char *msg;                  /* the refusal, if nothing covers it */
} CRenHit;
typedef struct CRenInactive {
    CRenHit *hits;
    size_t nhits;
    char **reads;               /* the files the unit's index has */
    size_t nreads;
} CRenInactive;
void c_rename_inactive_free(CRenInactive *x);
/* NULL: every hit of unit is in a file that one of the others (units that
 * passed) reads and compiles there; else the refusal for the first that none
 * does (malloc'd). */
char *c_rename_uncovered(const CRenInactive *unit, const CRenInactive *const *others,
                         size_t n, const CRenHit **hit);

typedef struct CRename {
    Options *o;                 /* the unit's */
    const char *main;           /* its main file */
    bool (*overlay)(void *ctx, const char *path, const char **buf,
                    size_t *len);   /* editor buffers, or NULL */
    void *overlay_ctx;
    const char *name;           /* the new name */
    /* The edits, one list per file, either given (files and old set: the
     * project's plan, B5d; the files the unit does not read are ignored) or
     * planned from the unit (files NULL): the name at offset off of path,
     * limited to the main file.  c_rename then sets files (one entry) and
     * own; c_rename_free releases it. */
    CRenFile *files;
    size_t nfiles;
    const char *old;            /* in, with files: the old name */
    const char *path;           /* the asked name is at offset off of this file (planning;
                                   with files, the entity to rename when the unit has
                                   it there, else the first an edit names) */
    uint32_t off;
    CRenInactive *inact;        /* out, if given: the inactive uses, not refused */
    uint32_t len;               /* out: the old name's length */
    bool own;                   /* out: files was planned here */
} CRename;
/* NULL: the edits stand; else why the rename is refused (malloc'd). */
char *c_rename(CRename *q);
/* For a refused inactive hit of the unit q names: what would turn the #if group
 * around it on in that unit (S4; the presence conditions of analysis/pc.h), one
 * line, malloc'd; NULL: no group there or the unit could not be read. */
char *c_rename_explain(const CRename *q, const CRenHit *hit);
void c_rename_free(CRename *q);

#endif
