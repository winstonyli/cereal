/* driver.h - one translation unit's worth of state, shared by all modes. */
#ifndef CEREAL_DRIVER_H
#define CEREAL_DRIVER_H

#include "common.h"
#include "diag.h"
#include "pp.h"
#include "srcmgr.h"

typedef struct CmdlineMacro {
    char kind;          /* 'D', 'U', 'i' (-include) */
    const char *text;
} CmdlineMacro;

typedef struct Options {
    PPOptions pp;
    VEC(CmdlineMacro) macros;
    VEC(const char *) wflags;
    DiagConfig *diag;   /* built from wflags by options_finish */
    VEC(const char *) inputs;
    const char *output;
    bool linemarkers;
    bool json;
    bool color;
    bool pedantic_errors;
    bool short_enums;           /* -fshort-enums */
    bool lax_vector;            /* -flax-vector-conversions */
    bool cf_nobranch;           /* -fcf-protection=none|return */
    bool show_system;
    bool no_warnings;           /* -w */
    bool fatal_errors;          /* -Wfatal-errors */
    int bad_options;            /* command-line errors reported */
    FILE *msg;                  /* where option errors and notes go (stderr; NULL: nowhere) */
    const char *cwd;            /* absolute working directory of the TU (NULL: the process's) */
    Arena rsp;                  /* strings from @files that the options point into */
    int std_year;               /* -std=: 1999, 2011 or 2017 */
    bool lenient;               /* skip what is not understood without notes (language server) */
    VEC(const char *) ignored_semantic; /* skipped, may change diagnostics */
    VEC(const char *) ignored_deps;     /* -MD ...: no dependency file written */
    bool track0;                /* -ftrack-macro-expansion=0 */
    bool trigraphs_flag;        /* -trigraphs given (else only the ISO -std= has them) */
    char opt_level;     /* '0', '1', '2', '3', 's', 'g', 'z' */
    char strict_alias;  /* -fstrict-aliasing 1, -fno-strict-aliasing 2 */
    bool check_versions;
    char parallel;      /* 'a'uto, 'y' on (forced), 'n' off */
    int par_threads;    /* <= 0: cpu_count() */
    size_t par_chunk;   /* 0: default */
    unsigned par_window;
    int jobs;           /* -j: concurrent TUs (<= 0: cpu_count()) */
    uint64_t fingerprint; /* of the flags (language server: cell caches) */
} Options;

typedef struct TU {
    Arena arena;
    VEC(Arena) adopted;  /* parallel workers' arenas that results live in */
    Interner *in;        /* reference counted: may be shared by builds */
    SrcMgr sm;
    DiagEngine diag;
    PP pp;
    Options *opt;
} TU;

/* Parse common options starting at argv[i]; returns number consumed (0 if
 * not a common option, -1 on error). */
int options_parse_one(Options *o, int argc, char **argv, int i);
/* Does argv[i] change what cereal reports?  False for the options it skips
 * without effect (-c, -fPIC, -MD, ...: not -fno-common and the like, which
 * are noted); for fingerprints of flags. */
bool option_affects_diagnostics(const char *a);
/* An option problem: "cereal: error: <text>\n" to o->msg, counted in
 * bad_options.  Option parsing never exits and never writes elsewhere. */
void opt_error(Options *o, const char *fmt, ...);
void options_init(Options *o);
void options_finish(Options *o);   /* add host dirs etc. */
void options_free(Options *o);

void tu_init(TU *tu, Options *opt);
/* The same, interning into `in` (retained), e.g. one interner for all
 * builds of an edited unit so identifier ids stay stable. */
void tu_init_shared(TU *tu, Options *opt, Interner *in);
bool tu_begin(TU *tu, const char *path);
void tu_free(TU *tu);
/* Run the preprocessor to the end, discarding output. */
void tu_drain(TU *tu);

#endif
