/* par.h - two-phase parallel preprocessing (docs/PARALLEL.md). */
#ifndef CEREAL_PAR_H
#define CEREAL_PAR_H

#include "driver.h"

typedef struct ParOptions {
    int threads;             /* <= 0: cpu_count() */
    size_t chunk;            /* max segment bytes before a split */
    bool force;              /* use workers even for small inputs */
    size_t min_bytes;        /* auto mode: minimum text for parallelism */
    unsigned window;         /* segments after a worker's start at which a
                                predecessor may stitch (0: default) */
} ParOptions;

typedef enum {
    PAR_DONE,                /* output written */
    PAR_FALLBACK,            /* not attempted or diverged: run sequentially */
    PAR_FAILED               /* input could not be opened */
} ParResult;

/* `-E` for one TU.  tu must be tu_init'ed but not begun.  On PAR_FALLBACK
 * nothing has been written and the TU must be discarded. */
ParResult par_write_output(TU *tu, const char *path, FILE *out,
                           bool linemarkers, const ParOptions *po);

#endif
