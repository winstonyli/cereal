/* compdb.c - compile_commands.json entries and the options of one entry. */
#include "compdb.h"

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hash.h"
#include "json.h"

static bool split_space(char c, bool gcc)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           (gcc && (c == '\v' || c == '\f'));
}

int split_args(Arena *a, const char *s, SplitMode mode, const char ***argv)
{
    VEC(const char *) v = {0};
    StrBuf sb = {0};
    bool gcc = mode == SPLIT_GCC;
    int n;
    for (;;) {
        char q = 0;
        bool any = gcc, esc = false;   /* every gcc token is an argument */
        while (split_space(*s, gcc))
            s++;
        if (!*s)
            break;
        sb.len = 0;
        while (*s && (q || esc || !split_space(*s, gcc))) {
            if (gcc) {
                /* libiberty buildargv: a backslash escapes the next
                 * character even inside quotes; quotes only group */
                if (esc) {
                    sb_putc(&sb, *s);
                    esc = false;
                } else if (*s == '\\') {
                    esc = true;
                } else if (q) {
                    if (*s == q)
                        q = 0;
                    else
                        sb_putc(&sb, *s);
                } else if (*s == '"' || *s == '\'') {
                    q = *s;
                } else {
                    sb_putc(&sb, *s);
                }
            } else if (q) {
                if (*s == q) {
                    q = 0;
                } else if (q == '"' && *s == '\\' && s[1] &&
                           strchr("\"\\$`\n", s[1])) {
                    sb_putc(&sb, *++s);
                } else {
                    sb_putc(&sb, *s);
                }
            } else if (*s == '"' || *s == '\'') {
                q = *s;
                any = true;
            } else if (*s == '\\' && s[1]) {
                sb_putc(&sb, *++s);
            } else {
                sb_putc(&sb, *s);
            }
            s++;
        }
        if (sb.len || any)
            vec_push(&v, arena_strndup(a, sb.data ? sb.data : "", sb.len));
    }
    *argv = NEW_ARRAY(a, const char *, v.len + 1);
    if (v.len)
        memcpy(*argv, v.data, sizeof(char *) * v.len);
    n = (int)v.len;
    vec_free(&v);
    sb_free(&sb);
    return n;
}

/* Append argv[0..argc) to out, each `@file` replaced by the arguments in
 * the file, which are scanned again (nested @files).  `iter` counts the
 * expansions of the whole call, as libiberty's expandargv does. */
static bool expand_into(Arena *a, const char *dir, StrVec *out,
                        const char *const *argv, int argc, int *iter,
                        const char **err)
{
    int i;
    for (i = 0; i < argc; i++) {
        const char *p = argv[i], *path, **sub;
        char *text;
        size_t len;
        struct stat st;
        int n;
        if (p[0] != '@') {
            vec_push(out, p);
            continue;
        }
        if (++*iter >= 2000) {
            *err = "too many @-files encountered";
            return false;
        }
        path = p[1] == '/' || !dir ? p + 1 : arena_printf(a, "%s/%s", dir, p + 1);
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            *err = "@-file refers to a directory";
            return false;
        }
        text = p[1] ? read_file(path, &len) : NULL;
        if (!text) {
            *err = arena_printf(a, "cannot read response file '%s'", p + 1);
            return false;
        }
        n = split_args(a, text, SPLIT_GCC, &sub);
        free(text);
        if (!expand_into(a, dir, out, sub, n, iter, err))
            return false;
    }
    return true;
}

const char *const *argv_expand(Arena *a, const char *dir, int *argc, int from,
                               const char *const *argv, const char **err)
{
    StrVec v = {0};
    const char **res;
    int iter = 0, i;
    for (i = from; i < *argc && argv[i][0] != '@'; i++)
        ;
    if (i >= *argc)
        return argv;                    /* nothing to expand */
    for (i = 0; i < from; i++)
        vec_push(&v, argv[i]);
    if (!expand_into(a, dir, &v, argv + from, *argc - from, &iter, err)) {
        vec_free(&v);
        return NULL;
    }
    res = NEW_ARRAY(a, const char *, v.len + 1);
    memcpy(res, v.data, sizeof(char *) * v.len);
    *argc = (int)v.len;
    vec_free(&v);
    return res;
}

const char *compdb_abs(Arena *a, const char *dir, const char *p)
{
    if (p[0] == '/' || !dir)
        return path_normalize(a, p);
    return path_normalize(a, arena_printf(a, "%s/%s", dir, p));
}

int compdb_load(Arena *a, const char *path, CompileDb *out, FILE *msg,
                int *skipped)
{
    size_t len, i;
    char *text = read_file(path, &len), *cwd;
    JsonValue *db;
    const char *dbdir;
    if (!text)
        return -1;
    db = json_parse(a, text, len);
    free(text);
    if (!db || db->kind != JV_ARR)
        return -2;
    cwd = getcwd(NULL, 0);
    dbdir = path_dirname(a, compdb_abs(a, cwd, path));
    free(cwd);
    *skipped = 0;
    for (i = 0; i < db->len; i++) {
        JsonValue *e = db->items[i], *args = json_get(e, "arguments"),
                  *cmd = json_get(e, "command");
        const char *dir = json_str_of(json_get(e, "directory"), NULL);
        const char *file = json_str_of(json_get(e, "file"), NULL);
        CompileEntry ce;
        const char *why = NULL;
        if (!file)
            why = "has no 'file'";
        else if (!(args && args->kind == JV_ARR) && !(cmd && cmd->kind == JV_STR))
            why = "has neither 'arguments' nor 'command'";
        if (why) {
            if (msg)
                fprintf(msg, "cereal: error: %s: entry %zu %s, skipped\n", path,
                        i + 1, why);
            ++*skipped;
            continue;
        }
        memset(&ce, 0, sizeof ce);
        ce.dir = dir ? compdb_abs(a, dbdir, dir) : dbdir;
        ce.file = file;
        ce.abs_file = compdb_abs(a, ce.dir, file);
        if (args && args->kind == JV_ARR) {
            size_t k;
            ce.argv = NEW_ARRAY(a, const char *, args->len + 1);
            for (k = 0; k < args->len; k++)
                ce.argv[ce.argc++] = json_str_of(args->items[k], "");
        } else {
            ce.argc = split_args(a, cmd->str, SPLIT_SHELL, &ce.argv);
        }
        vec_push(out, ce);
    }
    return 0;
}

/* The entry's arguments with @files expanded (relative to its directory);
 * strings and array in `a`.  On failure the arguments as written, with *err
 * set. */
static const char *const *entry_expand(Arena *a, const CompileEntry *e,
                                       int *ac, const char **err)
{
    const char *const *av = argv_expand(a, e->dir, ac, 1,
                                        (const char *const *)e->argv, err);
    if (av)
        return av;
    *ac = e->argc;
    return (const char *const *)e->argv;
}

bool entry_is_c(const CompileEntry *e)
{
    Arena scratch;
    const char *lang = NULL, *dot, *err = NULL;
    const char *const *argv;
    int i, argc = e->argc;
    bool is_c = false;
    arena_init(&scratch);
    argv = entry_expand(&scratch, e, &argc, &err);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-x") && i + 1 < argc)
            lang = argv[++i];
        else if (!strncmp(a, "-x", 2) && a[2])
            lang = a + 2;
        else if (!strcmp(a, "-o"))
            i++;
        else if (a[0] != '-' && !strcmp(a, e->file))
            break;                      /* -x only counts before the source */
    }
    if (lang && strcmp(lang, "none")) {
        is_c = !strcmp(lang, "c");
    } else {
        dot = strrchr(e->file, '.');
        is_c = dot && !strcmp(dot, ".c");
    }
    arena_free(&scratch);
    return is_c;
}

static const char *const path_opts[] = {"-I", "-iquote", "-isystem",
                                        "-include", NULL};

void add_flag(Arena *a, StrVec *v, const char *dir, const char **argv,
              int argc, int *i)
{
    const char *f = argv[*i];
    int k;
    for (k = 0; path_opts[k]; k++) {
        size_t n = strlen(path_opts[k]);
        if (strncmp(f, path_opts[k], n) != 0)
            continue;
        if (f[n]) { /* -Ipath */
            vec_push(v, path_opts[k]);
            vec_push(v, compdb_abs(a, dir, f + n));
            return;
        }
        if (*i + 1 < argc) { /* -I path */
            vec_push(v, path_opts[k]);
            vec_push(v, compdb_abs(a, dir, argv[++*i]));
            return;
        }
    }
    vec_push(v, f);
}

/* One list of arguments into o; skipped words are not hashed into *fp. */
static void parse_args(Options *o, const char *const *argv, int argc,
                       int from, uint64_t *fp, uint64_t *seq)
{
    int i, n, k;
    for (i = from; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1])
            continue;                   /* the source file, wrapper words */
        if (!strcmp(a, "-E") || !strcmp(a, "-fsyntax-only"))
            continue;
        if (!strncmp(a, "-o", 2)) {     /* -o X or -oX: the object */
            if (!a[2])
                i++;
            continue;
        }
        n = options_parse_one(o, argc, (char **)argv, i);
        if (n <= 0) {
            if (!o->lenient)
                opt_error(o, "unrecognized command-line option '%s'", a);
            n = 1;                      /* a compile command is written for
                                         * some compiler, not for cereal */
        }
        if (option_affects_diagnostics(a))
            for (k = 0; k < n; k++)
                *fp = hash64_mix(*fp, hash64_str(argv[i + k], (*seq)++));
        i += n - 1;
    }
}

void entry_options(Options *o, const CompileEntry *e,
                   const char *const *extra, int nextra)
{
    uint64_t fp = hash64_str(e->dir ? e->dir : "", 0), seq = 1;
    const char *const *av;
    const char *err = NULL;
    int ac = e->argc;
    o->cwd = e->dir;
    /* the strings live as long as o, which keeps pointers into them */
    av = entry_expand(&o->rsp, e, &ac, &err);
    if (err)                            /* the entry is not checked */
        opt_error(o, "%s", err);
    parse_args(o, av, ac, 1, &fp, &seq);
    parse_args(o, extra, nextra, 0, &fp, &seq);
    vec_free(&o->inputs);               /* a stray "-" is not an input */
    vec_free(&o->ignored_deps);         /* no dependency output, no note */
    options_finish(o);
    o->fingerprint = fp;
}
