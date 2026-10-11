/* jobs.h - the translation-unit job plumbing of main.c, shared with
 * `cereal fix` (fixmode.c): a job per input or compile_commands entry, run
 * up to -j at a time with output committed in input order. */
#ifndef CEREAL_JOBS_H
#define CEREAL_JOBS_H

#include "compdb.h"
#include "driver.h"

#define TU_UNUSED __attribute__((unused))

/* One translation unit of a mode: text to out, diagnostics to err; res is
 * the job's own result slot (TuJob.res). */
typedef int (*TuFn)(Options *o, const char *path, FILE *out, FILE *err,
                    void *res);

struct Batch;

typedef struct TuJob {
    struct Batch *batch;
    Options *o;                    /* the run's options */
    TuFn fn;
    const char *path;
    /* a compile_commands entry: the job runs with the options of the entry
     * and cereal's own (`extra`), in the entry's directory; else with `o` */
    const CompileEntry *entry;
    const char *const *extra;
    int nextra;
    void *res;                     /* handed to fn */
    char *out, *err;               /* buffered, written in input order */
    size_t out_len, err_len;
    int rc;
    bool finished;                 /* guarded by batch->m */
} TuJob;

/* The jobs (path, fn, entry set by the caller), up to -j at a time.
 * Output and diagnostics appear in job order, exactly as a one-at-a-time
 * run would print them; dirs: say when the directory changes.  Returns the
 * or of the jobs' statuses (left in jobs[i].rc).  A job array may be run
 * again. */
int run_jobs(Options *o, TuJob *jobs, size_t n, FILE *out, bool dirs);

/* The C entries of a compile_commands.json, planned (mode_compdb). */
typedef struct CdbPlan {
    Arena arena;
    CompileDb db;
    StrVec extra;                  /* cereal's own options, for each entry */
    TuJob *jobs;                   /* one per entry to run */
    size_t njobs;
    size_t nsel, n_notc, n_dup, n_not;
} CdbPlan;

/* 0, or 2 (a usage error, reported).  Entries unchecked or not C are only
 * counted.  fn is set on each job. */
int compdb_plan(Options *o, TuFn fn, const char *dbpath, const StrVec *own,
                CdbPlan *p);
void compdb_plan_free(CdbPlan *p);

#endif
