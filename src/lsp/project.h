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

#endif
