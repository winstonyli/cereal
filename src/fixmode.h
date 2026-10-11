/* fixmode.h - `cereal fix` (fixmode.c; docs/C_DESIGN.md section 6). */
#ifndef CEREAL_FIXMODE_H
#define CEREAL_FIXMODE_H

#include "compdb.h"
#include "driver.h"
#include "fixres.h"

/* --max-passes default; also the language server's fix-all. */
enum { FIX_MAX_PASSES = 8 };

typedef struct FixOpts {
    bool unsafe;             /* --unsafe: MAYBE fixes too */
    bool diff;               /* --diff: print the patch, change nothing */
    bool dry;                /* --dry-run: report only */
    bool apply;              /* --apply (the default) */
    int max_passes;          /* --max-passes=N (default 8) */
    const char *only;        /* --only=ID[,ID...] */
    const char *root;        /* --root=DIR (default: the working directory) */
    const char *edits;       /* --edits=FILE: the builder's test input */
} FixOpts;

/* One fix argument: 1 taken, 0 not a fix option, -1 a bad value (reported). */
int fix_flag(FixOpts *fx, const char *arg);
/* `cereal fix` over the inputs of o, or the entries of the database ccdb
 * (own: cereal's options for each entry).  The exit status. */
int fix_run(Options *o, const char *ccdb, const StrVec *own, const FixOpts *fx);

/* The language server's fix-all: the edits `cereal fix` would make to file,
 * a file of the unit main, with the texts from base, the editor buffers:
 * passes until stable (at most FIX_MAX_PASSES), the same gate.  Only edits
 * of file, as of the text in base (one edit if it took more than one pass).
 * False: main could not be read.  Free with fix_edits_free. */
typedef struct FixTextEdit {
    uint32_t begin, end;     /* byte offsets in the buffer */
    char *text;
} FixTextEdit;
typedef VEC(FixTextEdit) FixTextEdits;
void fix_edits_free(FixTextEdits *e);
bool fix_unit_edits(Options *o, const char *main, const char *file,
                    FixBaseFn base, void *base_ctx, FixTextEdits *out);

#endif
