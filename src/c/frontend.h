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
} FrontendOpts;

/* Parses (and checks) `path` into tu->diag.  tu must be freshly
 * tu_init'ed (an overlay or pp.cancel set on it is honoured), and stays
 * the caller's: nothing is printed or freed.  A set pp.cancel flag ends
 * the run early, with partial diagnostics.  False: the file could not be
 * opened. */
bool frontend_run(TU *tu, const char *path, const FrontendOpts *fo);

#endif
