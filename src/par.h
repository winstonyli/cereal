/* par.h - two-phase parallel preprocessing (docs/PARALLEL.md). */
#ifndef CEREAL_PAR_H
#define CEREAL_PAR_H

#include "driver.h"
#include "parclient.h"
#include "thread.h"

typedef struct ParOptions {
    int threads;             /* <= 0: cpu_count() */
    size_t chunk;            /* max segment bytes before a split */
    bool force;              /* use workers even for small inputs */
    size_t min_bytes;        /* auto mode: minimum text for parallelism */
    unsigned window;         /* segments after a worker's start at which a
                                predecessor may stitch (0: default) */
    ThreadPool *pool;        /* shared pool (NULL: a private one) */
    size_t size_hint;        /* main file size if not the one on disk
                                (an editor buffer); 0: stat the file */
    struct CellCache *cells; /* reuse and store phase-B results (cell.h);
                                no -E output with a cache */
    uint64_t cell_config;    /* fingerprint of what else the results depend
                                on (options); a change empties the cache */
} ParOptions;

typedef enum {
    PAR_DONE,                /* output written */
    PAR_FALLBACK,            /* not attempted or diverged: run sequentially */
    PAR_FAILED               /* input could not be opened */
} ParResult;


/* One TU, two-phase.  tu must be tu_init'ed (clients attached) but not
 * begun.  out == NULL: no -E output.  On PAR_FALLBACK nothing has been
 * written and the TU, clients included, must be discarded. */
ParResult par_run(TU *tu, const char *path, FILE *out, bool linemarkers,
                  const ParOptions *po, const ParClient *clients,
                  int nclients);

/* `-E` for one TU (par_run with no clients). */
ParResult par_write_output(TU *tu, const char *path, FILE *out,
                           bool linemarkers, const ParOptions *po);

/* auto mode: is the input worth a parallel run? (cheap: a stat) */
bool par_worth_it(const char *path, const ParOptions *po);

#endif
