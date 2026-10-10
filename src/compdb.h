/* compdb.h - compile_commands.json entries and the options of one entry
 * (docs/A4_DESIGN.md).  Used by `check --compile-commands` and the
 * language server. */
#ifndef CEREAL_COMPDB_H
#define CEREAL_COMPDB_H

#include "driver.h"

typedef VEC(const char *) StrVec;

typedef struct CompileEntry {
    const char *dir;         /* working directory: absolute, normalized (NULL: none) */
    const char *file;        /* as written */
    const char *abs_file;    /* absolute, normalized */
    const char **argv;
    int argc;
} CompileEntry;

typedef VEC(CompileEntry) CompileDb;

/* SPLIT_SHELL: a POSIX shell's quotes and backslashes (a backslash inside
 * single quotes is literal, in double quotes it escapes only " \ $ ` and
 * newline).  SPLIT_GCC: libiberty's buildargv, used for response files (a
 * backslash escapes the next character everywhere, quotes only group). */
typedef enum { SPLIT_SHELL, SPLIT_GCC } SplitMode;
int split_args(Arena *a, const char *s, SplitMode mode, const char ***argv);

/* Response files: argv[from..*argc) with every argument that starts with
 * `@` replaced by the arguments in that file (gcc's rules; the expansion is
 * rescanned, so @files nest; a relative name is relative to `dir`, else the
 * process directory, also when the naming file is elsewhere; a file with
 * only white space adds nothing, a `''` adds an empty argument).  Returns the new
 * argument array (strings and array in `a`; `argv` itself when there is no
 * @file) and sets *argc.  On failure returns NULL with *err set: the file
 * cannot be read (gcc would keep the argument as written; here that would
 * drop the flags silently), it is a directory, or 2000 files were read. */
const char *const *argv_expand(Arena *a, const char *dir, int *argc, int from,
                               const char *const *argv, const char **err);

/* `p` against `dir` (when relative and dir is given), normalized. */
const char *compdb_abs(Arena *a, const char *dir, const char *p);

/* Read the entries of the database at `path` (strings in `a`).  A relative
 * `directory` is taken from the database's own directory.  Returns 0, -1 if
 * the file cannot be read, -2 if it is not a JSON array.  An entry without
 * `file`, or without `arguments` and `command`, is skipped: reported to
 * `msg` (unless NULL) and counted in *skipped. */
int compdb_load(Arena *a, const char *path, CompileDb *out, FILE *msg,
                int *skipped);

/* Is the entry C?  gcc's rule: the last -x LANG before the source, else
 * the suffix of the file. */
bool entry_is_c(const CompileEntry *e);

/* A path option (-I -iquote -isystem -include) of argv[*i] pushed with its
 * relative path made absolute against `dir`; any other argument as is. */
void add_flag(Arena *a, StrVec *v, const char *dir, const char **argv,
              int argc, int *i);

/* The options of one entry, set on a fresh options_init'ed `o`: the
 * entry's arguments (its source, -E, -o and wrapper words skipped), then
 * `extra` (arguments already made absolute with add_flag; later wins), then
 * options_finish.  `o->cwd` is the entry's directory, so relative paths in
 * all of them resolve there.  Never writes dependency output.  Problems go
 * to o->msg and count in o->bad_options (unknown options too, unless
 * o->lenient).  Allocates nothing from shared arenas: safe on any thread.
 * o->fingerprint covers the directory and every argument that can change a
 * diagnostic. */
void entry_options(Options *o, const CompileEntry *e,
                   const char *const *extra, int nextra);

#endif
