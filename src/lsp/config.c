/* config.c - per-file options for the language server.
 *
 * Layers, later wins: the file's entry in compile_commands.json (else the
 * entry of the nearest file: headers and new files have none), then the
 * flags in .cereal files from the workspace root down to the file's
 * directory.  A .cereal file holds compiler flags, whitespace separated,
 * with # comments.  The entry's flags resolve against the entry's
 * directory; the .cereal flags are made absolute against the .cereal
 * file's (compdb.c). */
#include "lsp.h"

#include <string.h>

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

static void load_db(LspConfig *c, const char *path)
{
    int skipped;
    int rc = compdb_load(&c->arena, path, &c->cmds, NULL, &skipped);
    if (rc == -2)
        fprintf(stderr, "cereal lsp: %s: not a compilation database\n", path);
    if (rc == 0)
        c->db_path = arena_strdup(&c->arena, path);
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
static const CompileEntry *command_for(LspConfig *c, const char *path)
{
    const CompileEntry *best = NULL;
    size_t i, best_n = 0;
    for (i = 0; i < c->cmds.len; i++) {
        size_t n;
        if (!strcmp(c->cmds.data[i].abs_file, path))
            return &c->cmds.data[i];
        n = common_prefix(c->cmds.data[i].abs_file, path);
        if (!best || n > best_n) {
            best = &c->cmds.data[i];
            best_n = n;
        }
    }
    return best;
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
            n = split_args(a, line, SPLIT_SHELL, &args);
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
    const CompileEntry *cmd = command_for(c, path);
    CompileEntry none;
    StrVec flags = {0};
    memset(&none, 0, sizeof none);
    options_init(o);
    o->lenient = true;      /* a compile command is written for some compiler,
                             * not for cereal: what is not understood is skipped */
    add_cereal_files(&c->arena, &flags, c->root, path);
    entry_options(o, cmd ? cmd : &none, (const char *const *)flags.data,
                  (int)flags.len);
    /* the arguments must outlive o: keep them in the config arena */
    if (flags.len) {
        const char **keep = NEW_ARRAY(&c->arena, const char *, flags.len);
        memcpy(keep, flags.data, sizeof(char *) * flags.len);
    }
    vec_free(&flags);
    return o;
}

void config_options_free(Options *o)
{
    options_free(o);
    free(o);
}
