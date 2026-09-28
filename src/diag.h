/* diag.h - diagnostics engine. */
#ifndef CEREAL_DIAG_H
#define CEREAL_DIAG_H

#include "common.h"
#include "srcmgr.h"

typedef enum {
    DL_IGNORED,
    DL_NOTE,
    DL_REMARK,
    DL_WARNING,
    DL_ERROR,
    DL_FATAL
} DiagLevel;

typedef struct DiagNote {
    SrcLoc loc;
    const char *msg;
} DiagNote;

typedef struct Diagnostic {
    DiagLevel level;
    const char *id;          /* warning option name, "" for hard errors */
    SrcLoc loc;
    SrcRange range;          /* optional highlight (end == 0 if none) */
    const char *msg;
    VEC(DiagNote) notes;
    SrcLoc *inc_chain;       /* #include locations, innermost first */
    int ninc;
    const char *fixit;       /* optional suggested replacement text */
    uint32_t key;            /* plan item at the time (parallel ordering) */
} Diagnostic;

typedef enum { DIAG_FMT_TEXT, DIAG_FMT_JSON } DiagFormat;

/* -W configuration: built once from the command line, then read-only. */
typedef struct DiagConfig DiagConfig;
DiagConfig *diag_config_new(void);
void diag_config_free(DiagConfig *c);
/* "foo", "no-foo", "error", "group", "all", "everything"; false if unknown */
bool diag_config_apply(DiagConfig *c, const char *flag);
bool diag_config_werror(const DiagConfig *c);
bool diag_config_pedantic(const DiagConfig *c);

typedef struct DiagEngine {
    Arena *arena;
    SrcMgr *sm;
    VEC(Diagnostic *) all;
    int nerrors, nwarnings;
    bool werror;
    bool pedantic;
    bool pedantic_errors;
    bool show_system;        /* report warnings located in system headers */
    bool immediate;          /* print as reported (text format) */
    bool color;
    DiagFormat format;
    FILE *out;
    /* Supplies the current include chain (innermost first). */
    void (*include_chain)(void *ctx, SrcLoc **locs, int *n);
    void *include_chain_ctx;
    int max_errors;
    const DiagConfig *cfg;   /* NULL: defaults */
    uint32_t key;            /* current plan item (set by the preprocessor) */
} DiagEngine;

/* Warning option registry. */
typedef struct DiagOption {
    const char *name;
    const char *group;       /* hygiene | cond | include | pp | pedantic */
    DiagLevel level;         /* severity when enabled */
    bool on;                 /* enabled by default */
    const char *help;
} DiagOption;

void diag_init(DiagEngine *d, Arena *a, SrcMgr *sm);
void diag_free(DiagEngine *d);

DiagLevel diag_level_for(DiagEngine *d, const char *id, DiagLevel requested);
bool diag_enabled(DiagEngine *d, const char *id);

Diagnostic *diag_report(DiagEngine *d, DiagLevel lvl, const char *id,
                        SrcLoc loc, const char *fmt, ...);
Diagnostic *diag_vreport(DiagEngine *d, DiagLevel lvl, const char *id,
                         SrcLoc loc, const char *fmt, va_list ap);
void diag_note(DiagEngine *d, Diagnostic *dg, SrcLoc loc, const char *fmt, ...);
void diag_set_range(Diagnostic *dg, SrcLoc b, SrcLoc e);
/* Print a diagnostic now (used when immediate is false, e.g. after sort). */
void diag_print(DiagEngine *d, Diagnostic *dg);
void diag_flush(DiagEngine *d);   /* print everything not yet printed */
void diag_print_json(DiagEngine *d, FILE *out);
void diag_list_options(FILE *out);

const DiagOption *diag_find_option(const char *name);

#endif
