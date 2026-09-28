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
    VEC(const char *) inputs;
    const char *output;
    bool linemarkers;
    bool json;
    bool color;
    bool pedantic_errors;
    bool show_system;
    char opt_level;     /* '0', '1', '2', '3', 's', 'g', 'z' */
} Options;

typedef struct TU {
    Arena arena;
    Interner in;
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
bool tu_begin(TU *tu, const char *path);
void tu_free(TU *tu);
/* Run the preprocessor to the end, discarding output. */
void tu_drain(TU *tu);

#endif
