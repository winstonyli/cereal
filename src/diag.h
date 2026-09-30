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
    uint8_t once;            /* nonzero: report at most once per (once, loc);
                                a parallel merge drops later duplicates */
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
    /* option lookup cache keyed by the id string's address (ids are
     * literals); a hit is verified by name */
    const char *idc_key[128];
    int16_t idc_val[128];
} DiagEngine;

/* Warning option registry.  An option is enabled by an explicit -W flag,
 * else by an umbrella flag in `by` (gcc's EnabledBy), else by default.
 * A name ending in '=' is a gcc level option (-Wshift-overflow=): it is
 * spelled -Wname or -Wname=N (0: off) and printed as [-Wname=]. */
#define DO_ALL          0x01    /* -Wall */
#define DO_EXTRA        0x02    /* -Wextra */
#define DO_PEDANTIC     0x04    /* -pedantic, -Wpedantic */
#define DO_UNUSED       0x08    /* -Wunused (itself enabled by -Wall) */
#define DO_IMPLICIT     0x10    /* -Wimplicit (itself enabled by -Wall) */
#define DO_UNUSED_EXTRA 0x20    /* -Wunused and -Wextra together */

typedef struct DiagOption {
    const char *name;
    const char *group;       /* hygiene | cond | include | pp | pedantic | c */
    DiagLevel level;         /* severity when enabled */
    bool on;                 /* enabled by default */
    uint8_t by;              /* DO_* umbrella flags that enable it */
    const char *help;
} DiagOption;

void diag_init(DiagEngine *d, Arena *a, SrcMgr *sm);
void diag_free(DiagEngine *d);

DiagLevel diag_level_for(DiagEngine *d, const char *id, DiagLevel requested);
bool diag_enabled(DiagEngine *d, const char *id);
/* 1: enabled; 0: disabled by a flag (-Wno-X, or -Wno-all for an option
 * -Wall enables); -1: disabled by default.  gcc emits some pedwarns
 * untagged when the option is merely at its default (warn_return_type
 * == -1) and not at all when it is turned off. */
int diag_option_state(DiagEngine *d, const char *id);

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
