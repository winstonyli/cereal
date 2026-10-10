/* deps.c - the -M family (docs/A4_DESIGN.md section 6): one preprocessor
 * listener records the headers as gcc spells them, one writer prints the
 * make rule.  Attached by tu_begin, written by the mode's finish step. */
#include <errno.h>

#include "driver.h"

extern const char host_preinclude[];

typedef struct DepEnt {
    const char *key;    /* directory + name as the lookup spelled them */
    const char *id;     /* the same with the split kept (the dedup key) */
    bool sys;           /* found in a system directory or from a system header */
} DepEnt;

typedef struct Deps {
    PP *pp;
    const char *main;               /* the main file as written */
    VEC(const char *) stack;        /* spelled path of each open file */
    const char *cur;                /* what the last include event opened */
    VEC(DepEnt) list;               /* first lookup of each key, in order */
    uint32_t *slots;                /* hash set of the keys: index + 1 */
    size_t nslots;
    bool failed;                    /* a header was missing (and not -MG) */
} Deps;

/* True when `key` is new. */
static bool deps_add_key(Deps *d, const char *key)
{
    size_t i, h = hash_bytes(key, strlen(key));
    if (d->list.len * 2 >= d->nslots) {
        size_t n = d->nslots ? d->nslots * 2 : 64, k;
        free(d->slots);
        d->slots = xcalloc(n, sizeof *d->slots);
        d->nslots = n;
        for (k = 0; k < d->list.len; k++) {
            i = hash_bytes(d->list.data[k].id, strlen(d->list.data[k].id));
            while (d->slots[i & (n - 1)])
                i++;
            d->slots[i & (n - 1)] = (uint32_t)k + 1;
        }
    }
    for (i = h; d->slots[i & (d->nslots - 1)]; i++)
        if (!strcmp(d->list.data[d->slots[i & (d->nslots - 1)] - 1].id, key))
            return false;
    d->slots[i & (d->nslots - 1)] = (uint32_t)d->list.len + 1;
    return true;
}

/* gcc's name for the file an #include opened: the search directory (one
 * trailing slash dropped) or the includer's directory, joined with the
 * spelled name, unnormalized; a missing header (-MG) as spelled.  The
 * includer's directory comes from its spelled path, never from the presumed
 * name of a #line.  The command line is searched from "./". */
static const char *include_key(Deps *d, const IncludeEvent *ev, const char **id)
{
    PP *pp = d->pp;
    const char *dir = "";
    char kind = 'I';
    if (ev->result != INC_NOT_FOUND && ev->dir_index >= 0) {
        size_t n = strlen(pp->search.data[ev->dir_index]);
        dir = pp->search.data[ev->dir_index];
        if (n > 1 && dir[n - 1] == '/')
            n--;
        dir = arena_printf(pp->arena, "%.*s/", (int)n, dir);
        kind = 'S';
    } else if (ev->result != INC_NOT_FOUND && ev->spelled[0] != '/') {
        const char *inc = vec_last(&d->stack), *slash = strrchr(inc, '/');
        dir = arena_printf(pp->arena, "%.*s", slash ? (int)(slash - inc + 1) : 0, inc);
    }
    /* gcc caches by (directory object, name): the same path through two
     * directories (a search directory and an includer's) is listed twice */
    *id = arena_printf(pp->arena, "%c%s\001%s", kind, dir, ev->spelled);
    return arena_printf(pp->arena, "%s%s", dir, ev->spelled);
}

static void on_include(void *ctx, const IncludeEvent *ev)
{
    Deps *d = ctx;
    const char *key, *id;
    d->cur = NULL;
    if (ev->result == INC_SKIPPED_ONCE)
        return;
    if (ev->result == INC_NOT_FOUND && !pp_missing_ok(d->pp->opt, ev)) {
        d->failed = true;
        return;
    }
    key = include_key(d, ev, &id);
    d->cur = key;
    if (deps_add_key(d, id)) {
        DepEnt e = {key, id, ev->system};
        vec_push(&d->list, e);
    }
}

static void on_file_enter(void *ctx, SrcFile *f, const IncludeEvent *via)
{
    Deps *d = ctx;
    vec_push(&d->stack, via && d->cur ? d->cur
                        : f->kind == SF_VIRTUAL ? "./" : d->main);
}

static void on_file_exit(void *ctx, SrcFile *f)
{
    Deps *d = ctx;
    (void)f;
    d->stack.len--;
}

void deps_attach(TU *tu, const char *path)
{
    Deps *d = xcalloc(1, sizeof *d);
    PPListener l;
    d->pp = &tu->pp;
    d->main = path;
    tu->deps = d;
    memset(&l, 0, sizeof l);
    l.ctx = d;
    l.include = on_include;
    l.file_enter = on_file_enter;
    l.file_exit = on_file_exit;
    pp_add_listener(&tu->pp, l);
}

void deps_free(TU *tu)
{
    Deps *d = tu->deps;
    if (!d)
        return;
    vec_free(&d->stack);
    vec_free(&d->list);
    free(d->slots);
    free(d);
    tu->deps = NULL;
}

/* ---- the writer ----------------------------------------------------- */

/* gcc's quoting of a name for make: `$` doubles, `#` and white space get a
 * backslash (and the backslashes before white space double). */
static const char *make_quote(Arena *a, const char *s)
{
    StrBuf sb = {0};
    const char *p, *q;
    char *r;
    for (p = s; *p; p++) {
        if (*p == ' ' || *p == '\t') {
            for (q = p; q > s && q[-1] == '\\'; q--)
                sb_putc(&sb, '\\');
            sb_putc(&sb, '\\');
        } else if (*p == '$') {
            sb_putc(&sb, '$');
        } else if (*p == '#') {
            sb_putc(&sb, '\\');
        }
        sb_putc(&sb, *p);
    }
    r = arena_strndup(a, sb_cstr(&sb), sb.len);
    sb_free(&sb);
    return r;
}

/* The name as gcc stores a prerequisite: leading "./" (and the slashes
 * after it) dropped. */
static const char *strip_dot(const char *p)
{
    while (p[0] == '.' && p[1] == '/') {
        p += 2;
        while (*p == '/')
            p++;
    }
    return p;
}

/* Names after the first go on the line unless it would pass column 72. */
static void put_names(StrBuf *sb, size_t *col, const char *const *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        size_t len = strlen(v[i]);
        if (*col) {
            if (*col + len > 72) {
                sb_puts(sb, " \\\n ");
                *col = 1;
            } else {
                sb_putc(sb, ' ');
                (*col)++;
            }
        }
        sb_puts(sb, v[i]);
        *col += len;
    }
}

/* The input's base name with its last suffix cut. */
static char *stem(Arena *a, const char *path)
{
    const char *b = strrchr(path, '/'), *dot;
    char *r;
    b = b ? b + 1 : path;
    r = arena_strdup(a, b);
    dot = strrchr(r, '.');
    if (dot)
        r[dot - r] = '\0';
    return r;
}

/* Where -MD / -MMD put the rule: -o's name with its last suffix replaced,
 * else the input's base name with .d (with -c, -S or -E), else what the gcc
 * driver names it in link mode (which includes -fsyntax-only). */
static const char *md_file(Arena *a, const Options *o, const char *path)
{
    if (o->output) {
        const char *slash = strrchr(o->output, '/'), *dot = strrchr(o->output, '.');
        size_t n = dot && (!slash || dot > slash) ? (size_t)(dot - o->output)
                                                  : strlen(o->output);
        return arena_printf(a, "%.*s.d", (int)n, o->output);
    }
    if (o->stage)
        return arena_printf(a, "%s.d", stem(a, path));
    if (o->inputs.len == 1 && !strcmp(stem(a, path), "a"))
        return "a.d";
    return arena_printf(a, "a-%s.d", stem(a, path));
}

static bool terminated(TU *tu)
{
    size_t i;
    if (tu->pp.halted || tu->deps->failed)
        return true;
    if (tu->diag.fatal_errors && tu->diag.nerrors)
        return true;
    for (i = 0; i < tu->diag.all.len; i++)
        if (tu->diag.all.data[i]->level == DL_FATAL)
            return true;
    return false;
}

void deps_write(TU *tu, FILE *out)
{
    Deps *d = tu->deps;
    const Options *o = tu->opt;
    const DepsOpts *dp = &o->deps;
    Arena *a = &tu->arena;
    VEC(const char *) tg = {0}, pre = {0};
    StrBuf sb = {0};
    const char *dest, *path;
    size_t i, col = 0, nmt, nmq;
    bool user_only = dp->style == 2 || dp->style == 4;
    FILE *f = out;
    if (!d || terminated(tu))
        return;
    path = d->main;
    /* targets: -MT in order, then -MQ rotated left by (#-MT mod #-MQ) as
     * gcc-13 does (probed); else the default */
    nmt = dp->mt.len;
    nmq = dp->mq.len;
    for (i = 0; i < nmt; i++)
        vec_push(&tg, dp->mt.data[i]);
    for (i = 0; i < nmq; i++)
        vec_push(&tg, make_quote(a, dp->mq.data[(i + nmt) % nmq]));
    if (!tg.len)
        vec_push(&tg, make_quote(a, dp->style <= 2 && o->output && o->stage != 'E'
                                        ? o->output
                                        : arena_printf(a, "%s.o", stem(a, path))));
    /* prerequisites: the main file, the implicit preinclude (a system
     * file), then the headers in order of first lookup */
    vec_push(&pre, make_quote(a, strip_dot(path)));
    if (!user_only && !o->pp.nostdinc && !dp->freestanding && *host_preinclude)
        vec_push(&pre, make_quote(a, host_preinclude));
    for (i = 0; i < d->list.len; i++)
        if (!(user_only && d->list.data[i].sys) &&
            strcmp(d->list.data[i].key, host_preinclude))
            vec_push(&pre, make_quote(a, strip_dot(d->list.data[i].key)));
    put_names(&sb, &col, tg.data, tg.len);
    sb_putc(&sb, ':');
    col++;
    put_names(&sb, &col, pre.data, pre.len);
    sb_putc(&sb, '\n');
    for (i = 1; dp->phony && i < pre.len; i++)
        sb_printf(&sb, "%s:\n", pre.data[i]);
    dest = dp->file ? dp->file : dp->md ? md_file(a, o, path)
                     : dp->style >= 3 ? o->output : NULL;
    if (dest && !strcmp(dest, "-"))
        dest = NULL;
    if (dest && !(f = fopen(dest, "w"))) {
        diag_report(&tu->diag, DL_ERROR, "", 0, "cannot open '%s' for writing: %s",
                    dest, strerror(errno));
    } else {
        fwrite(sb.data, 1, sb.len, f);
        if (f != out)
            fclose(f);
    }
    vec_free(&tg);
    vec_free(&pre);
    sb_free(&sb);
}
