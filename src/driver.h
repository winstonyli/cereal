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
    bool show_system;
    bool no_warnings;           /* -w */
    bool fatal_errors;          /* -Wfatal-errors */
    int bad_options;            /* command-line errors reported */
    bool track0;                /* -ftrack-macro-expansion=0 */
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
