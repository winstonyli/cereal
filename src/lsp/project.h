/* project.h - the background project index of the language server
 * (docs/B5_DESIGN.md sections 8, 9, 13): one scheduler thread keeps the
 * store's manifests fresh for every translation unit of the compile
 * database, running index jobs on a low-priority pool (indexjob.h), in
 * process or in a child.
 *
 * The scheduler thread owns the project state (pstate.h).  The protocol
 * thread and the builder only post events (a mailbox under the project's
 * lock) and never wait on index work, except the test-only barrier. */
#ifndef CEREAL_PROJECT_H
#define CEREAL_PROJECT_H

#include "lsp.h"

/* The settings (13.2) are read from params.initializationOptions.index and
 * CEREAL_INDEX_<NAME>.  Starts the scheduler thread, which does nothing until
 * project_go (the `initialized` notification).  False: indexing is off (no
 * compile database, `enable` false, or no usable cache) and nothing else of
 * this API does anything. */
bool project_start(const JsonValue *params, LspConfig *cfg);
void project_go(void);
/* Cancels the jobs, joins the thread and the pool, frees everything. */
void project_stop(void);

/* Events from the server (cheap; any thread). */
void project_doc(const char *path, bool open);   /* didOpen, didClose */
void project_edit(void);                         /* didChange: cancel, pause */
void project_save(const char *path);             /* didSave */
/* A response to a request the project sent (progress create): true if it
 * was one of its own. */
bool project_response(const JsonValue *id);

/* Test requests: cereal/indexStats writes an object; cereal/indexWait blocks
 * the caller until the scheduler has nothing to do; cereal/indexHold parks
 * the jobs that start (and so keeps them running) while true. */
void project_stats(JsonWriter *w);
void project_wait(void);
void project_hold(bool hold);

/* The project's names for workspace/symbol (projidx.h); NULL when indexing is
 * off.  Valid until project_stop. */
struct ProjIndex *project_index(void);
/* A query found a file that is not what the index read: look at the files
 * again soon (7.3).  Any thread; cheap, and rate limited. */
void project_hint_sweep(void);

/* ---- rename (B5d; docs/B5_DESIGN.md 11.5) ---- */

typedef struct ProjScope {
    char *refuse;            /* R2 or R5: why the rename is refused (malloc'd), else NULL */
    char **mains;            /* the units that list any of the files, to check (malloc'd) */
    size_t *entries;         /* their compile database entries */
    char **overlays;         /* per unit: a virtual unit's overlay words, else NULL; these
                                follow the real units and only cover their inactive uses */
    size_t nmains;
} ProjScope;
/* Asks the scheduler which units a rename of names in `files` must check, and
 * whether the index is complete enough to rename (R2, R5).  timeout_ms 0:
 * wait.  False: no answer (indexing off, shutdown, or the time ran out).
 * Free the answer with project_scope_free. */
bool project_scope(const char *const *files, size_t nfiles, unsigned timeout_ms,
                   ProjScope *out);
void project_scope_free(ProjScope *s);
/* While paused (calls nest) the scheduler submits no job: the rename's checks
 * have the pool. */
void project_pause(bool on);
/* Runs fn(arg, i) for i in [0, n) on the index pool and returns when all are
 * done; the caller helps.  Without a pool they run here, in turn. */
void project_gate(void (*fn)(void *arg, size_t i), void *arg, size_t n);

#endif
