/* config.c - per-file options for the language server.
 *
 * Layers, later wins: the file's entry in compile_commands.json (else the
 * entry of the nearest file: headers and new files have none), then the
 * flags in .cereal files from the workspace root down to the file's
 * directory.  A .cereal file holds compiler flags, whitespace separated,
 * with # comments. */
#include "lsp.h"

#include <string.h>

#include "../hash.h"

int shell_split(Arena *a, const char *s, const char ***argv)
{
    VEC(const char *) v = {0};
    StrBuf sb = {0};
    int n;
    for (;;) {
        char q = 0;
        bool any = false;
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
            s++;
        if (!*s)
            break;
        sb.len = 0;
        while (*s && (q || !(*s == ' ' || *s == '\t' || *s == '\n' ||
                             *s == '\r'))) {
            if (q) {
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

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    StrBuf sb = {0};
    char buf[1 << 16];
    size_t n;
    if (!f)
        return NULL;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        sb_putn(&sb, buf, n);
    fclose(f);
    *len = sb.len;
    if (!sb.data)
        return xcalloc(1, 1);
    return sb.data;
}

typedef VEC(const char *) StrVec;

static const char *abs_path(Arena *a, const char *dir, const char *p)
{
    if (p[0] == '/' || !dir)
        return path_normalize(a, p);
    return path_normalize(a, arena_printf(a, "%s/%s", dir, p));
}

static void load_db(LspConfig *c, const char *path)
{
    size_t len, i;
    char *text = slurp(path, &len);
    JsonValue *db;
    if (!text)
        return;
    db = json_parse(&c->arena, text, len);
    free(text);
    if (!db || db->kind != JV_ARR) {
        fprintf(stderr, "cereal lsp: %s: not a compilation database\n", path);
        return;
    }
    c->db_path = arena_strdup(&c->arena, path);
    for (i = 0; i < db->len; i++) {
        JsonValue *e = db->items[i], *args = json_get(e, "arguments");
        const char *dir = json_str_of(json_get(e, "directory"), NULL);
        const char *file = json_str_of(json_get(e, "file"), NULL);
        CompileCmd cmd;
        if (!file)
            continue;
        memset(&cmd, 0, sizeof cmd);
        cmd.dir = dir ? path_normalize(&c->arena, dir) : NULL;
        cmd.file = abs_path(&c->arena, cmd.dir, file);
        if (args && args->kind == JV_ARR) {
            size_t k;
            cmd.argv = NEW_ARRAY(&c->arena, const char *, args->len + 1);
            for (k = 0; k < args->len; k++)
                cmd.argv[cmd.argc++] = json_str_of(args->items[k], "");
        } else {
            cmd.argc = shell_split(&c->arena,
                                   json_str_of(json_get(e, "command"), ""),
                                   &cmd.argv);
        }
        vec_push(&c->cmds, cmd);
    }
}

void config_init(LspConfig *c, const char *root)
{
    static const char *const where[] = {"compile_commands.json",
                                        "build/compile_commands.json", NULL};
    int i;
    memset(c, 0, sizeof *c);
    arena_init(&c->arena);
    c->root = root ? path_normalize(&c->arena, root) : NULL;
    for (i = 0; c->root && where[i] && !c->db_path; i++) {
        const char *p = arena_printf(&c->arena, "%s/%s", c->root, where[i]);
        FILE *f = fopen(p, "rb");
        if (f) {
            fclose(f);
            load_db(c, p);
        }
    }
}

void config_free(LspConfig *c)
{
    vec_free(&c->cmds);
    arena_free(&c->arena);
}

static size_t common_prefix(const char *a, const char *b)
{
    size_t n = 0;
    while (a[n] && a[n] == b[n])
        n++;
    return n;
}

/* The file's own command, else the one whose file shares the longest
 * path prefix (same directory first): the flags a header is most likely
 * compiled with. */
static const CompileCmd *command_for(LspConfig *c, const char *path)
{
    const CompileCmd *best = NULL;
    size_t i, best_n = 0;
    for (i = 0; i < c->cmds.len; i++) {
        size_t n;
        if (!strcmp(c->cmds.data[i].file, path))
            return &c->cmds.data[i];
        n = common_prefix(c->cmds.data[i].file, path);
        if (!best || n > best_n) {
            best = &c->cmds.data[i];
            best_n = n;
        }
    }
    return best;
}

/* GCC options that take their value as the next argument and that cereal
 * does not model (skipped with the value). */
static bool takes_value(const char *a)
{
    static const char *const opts[] = {
        "-o", "-MF", "-MT", "-MQ", "-x", "-arch", "-target", "-Xclang",
        "-Xpreprocessor", "-Xassembler", "-Xlinker", "-aux-info", "-dumpdir",
        "-dumpbase", "-idirafter", "-iprefix", "-iwithprefix", "-imacros",
        "-isysroot", "--sysroot", "-L", "-l", "-T", "-u", "-z", NULL};
    int i;
    for (i = 0; opts[i]; i++)
        if (!strcmp(a, opts[i]))
            return true;
    return false;
}

static const char *const path_opts[] = {"-I", "-iquote", "-isystem",
                                        "-include", NULL};

/* Rewrite relative paths in -I-style flags against dir. */
static void add_flag(Arena *a, StrVec *v, const char *dir,
                     const char **argv, int argc, int *i)
{
    const char *f = argv[*i];
    int k;
    for (k = 0; path_opts[k]; k++) {
        size_t n = strlen(path_opts[k]);
        if (strncmp(f, path_opts[k], n) != 0)
            continue;
        if (f[n]) { /* -Ipath */
            vec_push(v, path_opts[k]);
            vec_push(v, abs_path(a, dir, f + n));
            return;
        }
        if (*i + 1 < argc) { /* -I path */
            vec_push(v, path_opts[k]);
            vec_push(v, abs_path(a, dir, argv[++*i]));
            return;
        }
    }
    vec_push(v, f);
}

static void add_cereal_files(Arena *a, StrVec *v, const char *root,
                             const char *path)
{
    /* directories from the root (or /) down to the file's */
    char *d = arena_strdup(a, path), *slash;
    VEC(char *) dirs = {0};
    size_t i;
    while ((slash = strrchr(d, '/')) != NULL) {
        *slash = 0;
        vec_push(&dirs, arena_strdup(a, *d ? d : "/"));
        if (root && !strcmp(*d ? d : "/", root))
            break;
        if (!*d)
            break;
    }
    for (i = dirs.len; i-- > 0;) {
        const char *cf = arena_printf(a, "%s/.cereal", strcmp(dirs.data[i], "/")
                                                          ? dirs.data[i] : "");
        size_t len;
        char *text = slurp(cf, &len), *line, *save = NULL;
        if (!text)
            continue;
        for (line = strtok_r(text, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            const char **args;
            char *hash = strchr(line, '#');
            int n, k;
            if (hash)
                *hash = 0;
            n = shell_split(a, line, &args);
            for (k = 0; k < n; k++)
                add_flag(a, v, dirs.data[i], args, n, &k);
        }
        free(text);
    }
    vec_free(&dirs);
}

Options *config_options_for(LspConfig *c, const char *path)
{
    Options *o = xcalloc(1, sizeof *o);
    const CompileCmd *cmd = command_for(c, path);
    StrVec flags = {0};
    int i, n;
    options_init(o);
    if (cmd) {
        for (i = 1; i < cmd->argc; i++) { /* argv[0]: the compiler */
            const char *a = cmd->argv[i];
            if (a[0] != '-') /* the source file and other inputs */
                continue;
            if (takes_value(a)) {
                i++;
                continue;
            }
            if (!strcmp(a, "-c") || !strcmp(a, "-E") || !strcmp(a, "-S"))
                continue;
            /* cereal is C99-only and would stop on other standards; keep
             * the GNU flavour, which changes predefined macros */
            if (!strncmp(a, "-std=", 5)) {
                if (strstr(a, "gnu"))
                    vec_push(&flags, "-std=gnu99");
                continue;
            }
            add_flag(&c->arena, &flags, cmd->dir, cmd->argv, cmd->argc, &i);
        }
    }
    add_cereal_files(&c->arena, &flags, c->root, path);
    n = (int)flags.len;
    for (i = 0; i < n; i++) {
        int used;
        if (!strncmp(flags.data[i], "-std=", 5) &&
            strcmp(flags.data[i], "-std=c99") &&
            strcmp(flags.data[i], "-std=gnu99"))
            continue; /* would be fatal: C99 only */
        used = options_parse_one(o, n, (char **)flags.data, i);
        if (used > 0)
            i += used - 1;
        /* unknown or malformed flags are ignored: a compile command is
         * written for some compiler, not for cereal */
    }
    for (i = 0; i < n; i++)
        o->fingerprint = hash64_mix(o->fingerprint,
                                    hash64_str(flags.data[i], (uint64_t)i));
    /* the arguments must outlive o: keep them in the config arena */
    if (flags.len) {
        const char **keep = NEW_ARRAY(&c->arena, const char *, flags.len);
        memcpy(keep, flags.data, sizeof(char *) * flags.len);
    }
    vec_free(&flags);
    options_finish(o);
    return o;
}

void config_options_free(Options *o)
{
    options_free(o);
    free(o);
}
