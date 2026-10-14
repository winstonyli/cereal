/* indexjob.h - indexing one translation unit into the store
 * (docs/B5_DESIGN.md 8.3).  The one function every executor runs: the index
 * pool's threads call it in the server, and `cereal index-summary` calls it
 * in a child process (section 9) or from the command line. */
#ifndef CEREAL_INDEXJOB_H
#define CEREAL_INDEXJOB_H

#include "lsp.h"

typedef struct IdxJob {
    LspConfig *cfg;
    long entry;              /* its compile database entry; < 0: the unit is `path`,
                              * with the options an open file gets (command_for) */
    const char *path;
    const char *dir;         /* the project directory to write to (NULL: nowhere) */
    FILE *dump;              /* the text form of 14.2 goes here (NULL: none) */
    uint64_t build_id;
    size_t max_bytes;        /* byte budget for the files read (0: none) */
    uint32_t *cancel;        /* raised to cancel; also by the budget */
} IdxJob;

typedef enum { IDX_DONE, IDX_CANCELLED, IDX_ERROR } IdxResult;

typedef struct IdxOut {
    IdxResult result;
    int tu_status;           /* IDX_DONE: the manifest's CTU_* status */
    uint64_t tukey;
    char msg[512];           /* IDX_ERROR */
} IdxOut;

/* Runs the job; may call fatal() (the pool traps it, a child exits). */
void index_unit(const IdxJob *j, IdxOut *out);

#endif
