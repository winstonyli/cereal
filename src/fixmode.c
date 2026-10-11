/* fixmode.c - `cereal fix` (docs/C_DESIGN.md section 6): collect the fix-its
 * of every translation unit, merge them (D5, D6), gate them by re-running
 * the units that read an edited file, write the files, and repeat.
 *
 * Also the builder's testing input,
 *     cereal fix --edits=FILE --diff
 * where FILE lists edit groups, one edit per line, groups separated by blank
 * lines (5.3):
 *     PATH BEGIN END "TEXT"
 * BEGIN and END are byte offsets into PATH; TEXT takes \\ \" \n \t \r and
 * \ooo.  Lines starting with # are comments.  The result is the unified
 * diff of the accepted edits on stdout; a skipped group is named on stderr. */
#include "fixmode.h"

#include "fixres.h"
#include "fixset.h"
#include "jobs.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* qsort, which an empty vector's NULL data may not be passed to */
static void sortn(void *base, size_t n, size_t size,
                  int (*cmp)(const void *, const void *))
{
    if (n > 1)
        qsort(base, n, size, cmp);
}

/* ---- --edits: the builder's input ------------------------------------------ */

typedef struct Src {
    const char *name;        /* as first written */
    char *buf;
    size_t len;
} Src;

/* TEXT between the quotes of s; false if malformed. */
static bool unquote(const char *s, StrBuf *out)
{
    if (*s++ != '"')
        return false;
    for (; *s && *s != '"'; s++) {
        if (*s != '\\') {
            sb_putc(out, *s);
            continue;
        }
        s++;
        if (*s == 'n')
            sb_putc(out, '\n');
        else if (*s == 't')
            sb_putc(out, '\t');
        else if (*s == 'r')
            sb_putc(out, '\r');
        else if (*s >= '0' && *s <= '7') {
            int v = 0, k;
            for (k = 0; k < 3 && s[k] >= '0' && s[k] <= '7'; k++)
                v = v * 8 + (s[k] - '0');
            sb_putc(out, (char)v);
            s += k - 1;
        } else if (*s == '\\' || *s == '"')
            sb_putc(out, *s);
        else
            return false;
    }
    return *s == '"' && !s[1];
}

static int cmp_src(const void *a, const void *b)
{
    return strcmp(((const Src *)a)->name, ((const Src *)b)->name);
}

static int edits_diff(const char *edits)
{
    FixSet fs;
    VEC(Src) srcs = {0};
    VEC(uint32_t) order = {0};
    Arena arena;
    char line[4096];
    size_t ln = 0, k;
    bool open = false;
    int rc = 0;
    FILE *ef = fopen(edits, "r");
    if (!ef) {
        fprintf(stderr, "cereal: error: cannot open '%s'\n", edits);
        return 2;
    }
    arena_init(&arena);
    fixset_init(&fs);
    while (fgets(line, sizeof line, ef)) {
        char *p = line, *path, *end;
        unsigned long b, e;
        StrBuf t = {0};
        char resolved[PATH_MAX];
        const char *key;
        uint32_t fi;
        ln++;
        p[strcspn(p, "\r\n")] = 0;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#')
            continue;
        if (!*p) {
            open = false;
            continue;
        }
        path = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (!*p)
            goto bad;
        *p++ = 0;
        b = strtoul(p, &end, 10);
        if (end == p)
            goto bad;
        p = end;
        e = strtoul(p, &end, 10);
        if (end == p)
            goto bad;
        p = end;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!unquote(p, &t))
            goto bad;
        if (b > e) {
            sb_free(&t);
            goto bad;
        }
        key = realpath(path, resolved) ? arena_strdup(&arena, resolved)
                                       : arena_strdup(&arena, path);
        fi = fixset_file(&fs, key);
        if (fi == srcs.len) {
            Src s;
            s.name = arena_strdup(&arena, path);
            s.buf = read_file(path, &s.len);
            if (!s.buf) {
                fprintf(stderr, "cereal: error: cannot read '%s'\n", path);
                sb_free(&t);
                rc = 2;
                goto done;
            }
            vec_push(&srcs, s);
        }
        if (!open) {
            fixset_group(&fs);
            open = true;
        }
        fixset_add(&fs, fi, (uint32_t)b, (uint32_t)e,
                   arena_strndup(&arena, t.data ? t.data : "", t.len));
        sb_free(&t);
        continue;
    bad:
        fprintf(stderr, "cereal: error: %s:%zu: expected PATH BEGIN END \"TEXT\"\n",
                edits, ln);
        rc = 2;
        goto done;
    }
    {
        size_t nskip = fixset_resolve(&fs);
        uint32_t g;
        if (nskip)
            for (g = 1; g <= fs.ngroups; g++)
                if (fs.skipped_by.data[g])
                    fprintf(stderr, "cereal: fix: group %u skipped: overlaps "
                            "group %u\n", g, fs.skipped_by.data[g]);
    }
    /* files by name, as the patch of gcc's edit context */
    for (k = 0; k < srcs.len; k++)
        vec_push(&order, (uint32_t)k);
    for (k = 0; k < order.len; k++) {
        size_t m;
        for (m = k + 1; m < order.len; m++)
            if (cmp_src(&srcs.data[order.data[m]], &srcs.data[order.data[k]]) < 0) {
                uint32_t t2 = order.data[k];
                order.data[k] = order.data[m];
                order.data[m] = t2;
            }
    }
    for (k = 0; k < order.len; k++) {
        Src *s = &srcs.data[order.data[k]];
        if (!fixset_diff(&fs, order.data[k], s->name, s->buf, s->len, stdout)) {
            fprintf(stderr, "cereal: error: %s: an edit crosses a line or lies "
                    "outside the file\n", s->name);
            rc = 1;
        }
    }
done:
    fclose(ef);
    for (k = 0; k < srcs.len; k++)
        free(srcs.data[k].buf);
    vec_free(&srcs);
    vec_free(&order);
    fixset_free(&fs);
    arena_free(&arena);
    return rc;
}

/* ---- options --------------------------------------------------------------- */

int fix_flag(FixOpts *fx, const char *a)
{
    if (!strcmp(a, "--unsafe"))
        fx->unsafe = true;
    else if (!strcmp(a, "--diff"))
        fx->diff = true;
    else if (!strcmp(a, "--dry-run"))
        fx->dry = true;
    else if (!strcmp(a, "--apply"))
        fx->apply = true;
    else if (!strncmp(a, "--max-passes=", 13)) {
        char *end;
        long v = strtol(a + 13, &end, 10);
        if (!a[13] || *end || v < 1 || v > 1000) {
            fputs("cereal: --max-passes needs a number from 1 to 1000\n", stderr);
            return -1;
        }
        fx->max_passes = (int)v;
    } else if (!strncmp(a, "--only=", 7))
        fx->only = a + 7;
    else if (!strncmp(a, "--root=", 7))
        fx->root = a + 7;
    else if (!strncmp(a, "--edits=", 8))
        fx->edits = a + 8;
    else
        return 0;
    return 1;
}

/* ---- the merge ---------------------------------------------------------------- */

/* A file read by some unit, by real path. */
typedef struct GFile {
    const char *key;
    const char *path;        /* how the first unit that read it spells it */
    char *text;             /* as the units read it (BOM dropped); malloc'd */
    size_t len;
    uint64_t hash;
    bool loaded, bom, crlf;
    const char *skip;        /* non-NULL: its edits are not applied, and why */
    bool skip_exit;          /* ... and that counts toward the exit status */
    VEC(uint32_t) rd;        /* the units that read it (result indexes) */
} GFile;

enum { CS_LIVE, CS_SKIP, CS_DUP };
enum { GM_STRICT, GM_PAREN, GM_LENIENT };

typedef struct CEdit {
    uint32_t gf, begin, end;
    const char *text;        /* with the file's line ending */
} CEdit;

typedef struct Cand {
    uint32_t tu, grp;        /* result index; group index in its groups */
    uint8_t cls;             /* 0 safe, 1 safe parentheses, 2 lenient */
    uint8_t state;
    uint32_t e0, ne;         /* its edits in Pass.ce */
    char *detail;
} Cand;

typedef struct Skip {
    char *path;
    uint32_t line, col;
    char *id, *why;
    bool exit_flag;
} Skip;

/* a unit's reported edits, for the question "did it report this one too" */
typedef struct EIdx {
    uint32_t gf, begin, end;
    const char *text, *id;
} EIdx;

typedef struct Pass {
    Options *o;
    const FixOpts *fx;
    TuJob *jobs;
    size_t n;
    FixResult *R;
    Arena arena;
    VEC(GFile) gf;
    uint32_t **gmap;         /* per unit: its file index -> GFile index */
    EIdx **eidx;
    size_t *neidx;
    VEC(Cand) cands;
    VEC(CEdit) ce;
    VEC(Skip) skips;
    const char *cwd, *root;
    bool io_error;
    bool probe;              /* only look: write nothing, count nothing */
    /* the language server's pass (fix_unit_edits): the texts are its buffers,
     * and only the edits of the file only_path are wanted */
    FixBaseFn base;
    void *base_ctx;
    const char *only_path;
} Pass;

static const char *disp(const Pass *P, const char *key)
{
    size_t n = strlen(P->cwd);
    if (!strncmp(key, P->cwd, n) && key[n] == '/')
        return key + n + 1;
    return key;
}

static void skip_add(Pass *P, const char *key, uint32_t line, uint32_t col,
                     const char *id, const char *why, bool exit_flag)
{
    Skip s;
    s.path = xstrdup(disp(P, key));
    s.line = line;
    s.col = col;
    s.id = xstrdup(*id ? id : "error");
    s.why = xstrdup(why);
    s.exit_flag = exit_flag;
    vec_push(&P->skips, s);
}

static const FGroup *cgroup(const Pass *P, const Cand *c)
{
    return &P->R[c->tu].groups.data[c->grp];
}

static void skip_cand(Pass *P, Cand *c, const char *why, bool exit_flag)
{
    const FixResult *r = &P->R[c->tu];
    const FGroup *g = cgroup(P, c);
    const char *key = g->pfile >= 0 ? r->files.data[g->pfile].key : "";
    c->state = CS_SKIP;
    skip_add(P, key, g->pline, g->pcol, g->id, why, exit_flag);
}

static uint64_t fnv(const char *s, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    while (n--)
        h = (h ^ (unsigned char)*s++) * 0x100000001B3ull;
    return h;
}

/* Reads the file the units read, once; sets gf->skip if it cannot be
 * changed, or is not what the units saw. */
static void gload(Pass *P, GFile *g)
{
    struct stat st;
    char *raw;
    size_t n, k;
    if (g->loaded)
        return;
    g->loaded = true;
    if (P->base) {                      /* the buffers: no file to check */
        const char *b;
        size_t bl;
        if (!P->base(P->base_ctx, g->path, &b, &bl)) {
            g->skip = "is not an open document";
            return;
        }
        raw = xmalloc(bl + 1);
        memcpy(raw, b, bl);
        raw[bl] = 0;
        n = bl;
        goto have;
    }
    {
        size_t rl = strlen(P->root);
        bool in = !strncmp(g->key, P->root, rl) &&
                  (!g->key[rl] || g->key[rl] == '/' || rl == 1);
        if (!in) {
            g->skip = "outside the root";
            return;
        }
    }
    if (stat(g->key, &st) != 0) {
        g->skip = "cannot be read";
        g->skip_exit = true;
        P->io_error = true;
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        g->skip = "is not a regular file";
        g->skip_exit = true;
        return;
    }
    if (st.st_nlink > 1) {
        g->skip = "has more than one hard link";
        g->skip_exit = true;
        return;
    }
    if (access(g->key, W_OK) != 0) {
        g->skip = "is read-only";
        g->skip_exit = true;
        return;
    }
    raw = read_file(g->key, &n);
    if (!raw) {
        g->skip = "cannot be read";
        g->skip_exit = true;
        P->io_error = true;
        return;
    }
    if (n >= 3 && !memcmp(raw, "\357\273\277", 3)) {
        g->bom = true;
        memmove(raw, raw + 3, n - 3);
        n -= 3;
        raw[n] = 0;
    }
have:
    g->text = raw;
    g->len = n;
    g->hash = fnv(raw, n);
    for (k = 0; k < n && raw[k] != '\n'; k++)
        ;
    /* an editor keeps the line ends of its text itself */
    g->crlf = !P->base && k < n && k > 0 && raw[k - 1] == '\r';
    for (k = 0; k < g->rd.len; k++) {
        const FixResult *r = &P->R[g->rd.data[k]];
        size_t f;
        for (f = 0; f < r->files.len; f++)
            if (!strcmp(r->files.data[f].key, g->key) &&
                r->files.data[f].hash != g->hash) {
                g->skip = "changed during the run";
                g->skip_exit = true;
                return;
            }
    }
}

typedef struct Trip {
    const char *key;
    uint32_t tu, lf;
} Trip;

static int trip_cmp(const void *pa, const void *pb)
{
    const Trip *a = pa, *b = pb;
    int c = strcmp(a->key, b->key);
    if (c)
        return c;
    if (a->tu != b->tu)
        return a->tu < b->tu ? -1 : 1;
    return a->lf < b->lf ? -1 : a->lf > b->lf;
}

static void build_table(Pass *P)
{
    VEC(Trip) t = {0};
    size_t i, k;
    for (i = 0; i < P->n; i++) {
        P->gmap[i] = xcalloc(P->R[i].files.len + 1, sizeof **P->gmap);
        for (k = 0; k < P->R[i].files.len; k++) {
            Trip x;
            x.key = P->R[i].files.data[k].key;
            x.tu = (uint32_t)i;
            x.lf = (uint32_t)k;
            vec_push(&t, x);
        }
    }
    if (t.len)
        sortn(t.data, t.len, sizeof *t.data, trip_cmp);
    for (i = 0; i < t.len; i++) {
        GFile *g;
        if (!P->gf.len || strcmp(P->gf.data[P->gf.len - 1].key, t.data[i].key)) {
            GFile n;
            memset(&n, 0, sizeof n);
            n.key = t.data[i].key;
            n.path = P->R[t.data[i].tu].files.data[t.data[i].lf].path;
            vec_push(&P->gf, n);
        }
        g = &P->gf.data[P->gf.len - 1];
        P->gmap[t.data[i].tu][t.data[i].lf] = (uint32_t)(P->gf.len - 1);
        if (!g->rd.len || g->rd.data[g->rd.len - 1] != t.data[i].tu)
            vec_push(&g->rd, t.data[i].tu);
    }
    vec_free(&t);
}

/* ---- did a unit report this edit ----------------------------------------------- */

static int eidx_cmp(const void *pa, const void *pb)
{
    const EIdx *a = pa, *b = pb;
    int c;
    if (a->gf != b->gf)
        return a->gf < b->gf ? -1 : 1;
    if (a->begin != b->begin)
        return a->begin < b->begin ? -1 : 1;
    if (a->end != b->end)
        return a->end < b->end ? -1 : 1;
    c = strcmp(a->text, b->text);
    return c ? c : strcmp(a->id, b->id);
}

static bool reported(Pass *P, size_t tu, const EIdx *key)
{
    if (!P->eidx[tu]) {
        const FixResult *r = &P->R[tu];
        size_t g, e, n = 0;
        EIdx *a = xmalloc((r->edits.len + 1) * sizeof *a);
        for (g = 0; g < r->groups.len; g++)
            for (e = 0; e < r->groups.data[g].ne; e++) {
                const FEdit *x = &r->edits.data[r->groups.data[g].e0 + e];
                a[n].gf = P->gmap[tu][x->file];
                a[n].begin = x->begin;
                a[n].end = x->end;
                a[n].text = x->text;
                a[n].id = r->groups.data[g].id;
                n++;
            }
        sortn(a, n, sizeof *a, eidx_cmp);
        P->eidx[tu] = a;
        P->neidx[tu] = n;
    }
    return P->neidx[tu] &&
           bsearch(key, P->eidx[tu], P->neidx[tu], sizeof *key, eidx_cmp);
}

static char *line_ending(Pass *P, const GFile *g, const char *text)
{
    StrBuf sb = {0};
    const char *p;
    char *r;
    if (!g->crlf || !strchr(text, '\n'))
        return (char *)text;
    for (p = text; *p; p++) {
        if (*p == '\n' && (p == text || p[-1] != '\r'))
            sb_putc(&sb, '\r');
        sb_putc(&sb, *p);
    }
    r = arena_strndup(&P->arena, sb.data, sb.len);
    sb_free(&sb);
    return r;
}

/* The diagnostic ids that can carry a fix-it ("" is spelled "error"): the
 * producers are listed in docs/C_DESIGN.md 4; tests/fix/only.sh runs each. */
static const char *const FIXABLE_IDS[] = {
    "error", "builtin-declaration-mismatch", "cond-typo", "format=",
    "header-guard", "implicit-function-declaration", "logical-not-parentheses",
    "macro-unparenthesized-body", "macro-unparenthesized-param",
    "missing-braces", "parentheses", "strict-prototypes", "xor-used-as-pow",
    NULL};

/* The first id of the --only list that no fix-it can carry, as a copy in
 * buf, or NULL if all are known. */
static const char *unknown_only_id(const char *only, char *buf, size_t cap)
{
    while (only) {
        const char *c = strchr(only, ',');
        size_t l = c ? (size_t)(c - only) : strlen(only), i;
        for (i = 0; FIXABLE_IDS[i]; i++)
            if (strlen(FIXABLE_IDS[i]) == l && !strncmp(FIXABLE_IDS[i], only, l))
                break;
        if (!FIXABLE_IDS[i]) {
            snprintf(buf, cap, "%.*s", (int)l, only);
            return buf;
        }
        only = c ? c + 1 : NULL;
    }
    return NULL;
}

static bool id_wanted(const char *only, const char *id)
{
    size_t n;
    if (!only)
        return true;
    if (!*id)
        id = "error";
    n = strlen(id);
    while (*only) {
        const char *c = strchr(only, ',');
        size_t l = c ? (size_t)(c - only) : strlen(only);
        if (l == n && !strncmp(only, id, n))
            return true;
        only += l + (c ? 1 : 0);
    }
    return false;
}

static int cand_cmp_sig(const Pass *P, const Cand *a, const Cand *b)
{
    uint32_t i;
    if (a->ne != b->ne)
        return a->ne < b->ne ? -1 : 1;
    for (i = 0; i < a->ne; i++) {
        const CEdit *x = &P->ce.data[a->e0 + i], *y = &P->ce.data[b->e0 + i];
        int c;
        if (x->gf != y->gf)
            return x->gf < y->gf ? -1 : 1;
        if (x->begin != y->begin)
            return x->begin < y->begin ? -1 : 1;
        if (x->end != y->end)
            return x->end < y->end ? -1 : 1;
        c = strcmp(x->text, y->text);
        if (c)
            return c;
    }
    return 0;
}

static const Pass *sort_pass;

static int perm_cmp(const void *pa, const void *pb)
{
    uint32_t a = *(const uint32_t *)pa, b = *(const uint32_t *)pb;
    int c = cand_cmp_sig(sort_pass, &sort_pass->cands.data[a],
                         &sort_pass->cands.data[b]);
    return c ? c : a < b ? -1 : a > b;
}

/* Which groups are candidates, in unit order and the order of their
 * diagnostics; then the checks that need no re-run: the file can be
 * written, D5, duplicates, conflicts. */
static void select_cands(Pass *P)
{
    size_t t, g, e;
    for (t = 0; t < P->n; t++) {
        const FixResult *r = &P->R[t];
        bool first_err = false;
        if (!r->opened)
            continue;
        for (g = 0; g < r->groups.len; g++) {
            const FGroup *fg = &r->groups.data[g];
            bool err = fg->level >= DL_ERROR;
            Cand c;
            if (fg->app == FIX_INFO || (fg->app == FIX_MAYBE && !P->fx->unsafe) ||
                !id_wanted(P->fx->only, fg->id))
                continue;
            if (P->only_path) {          /* a fix touching another file: not ours */
                bool other = false;
                for (e = 0; e < fg->ne; e++)
                    other |= strcmp(r->files.data[r->edits.data[fg->e0 + e].file].path,
                                    P->only_path) != 0;
                if (other)
                    continue;
            }
            memset(&c, 0, sizeof c);
            c.tu = (uint32_t)t;
            c.grp = (uint32_t)g;
            c.cls = err || fg->app == FIX_MAYBE ? 2 : fg->paren ? 1 : 0;
            c.e0 = (uint32_t)P->ce.len;
            vec_push(&P->cands, c);
            if (r->nerrors && !err) {
                skip_cand(P, &vec_last(&P->cands),
                          "deferred until the errors of this unit are fixed", true);
                continue;
            }
            if (err) {
                if (first_err) {
                    skip_cand(P, &vec_last(&P->cands), "held back: one error "
                              "fix per pass", true);
                    continue;
                }
                first_err = true;
            }
        }
    }
    /* edits, the file checks and D5 */
    for (t = 0; t < P->cands.len; t++) {
        Cand *c = &P->cands.data[t];
        const FixResult *r = &P->R[c->tu];
        const FGroup *fg = cgroup(P, c);
        const char *why = NULL;
        bool why_exit = true;
        if (c->state != CS_LIVE)
            continue;
        for (e = 0; e < fg->ne && !why; e++) {
            const FEdit *x = &r->edits.data[fg->e0 + e];
            GFile *gf = &P->gf.data[P->gmap[c->tu][x->file]];
            size_t k, m = 1, nrep = 1;
            EIdx key;
            gload(P, gf);
            if (gf->skip) {
                why = arena_printf(&P->arena, "%s %s", disp(P, gf->key), gf->skip);
                why_exit = gf->skip_exit;
                break;
            }
            if (x->begin > x->end || x->end > gf->len) {
                why = "has an edit outside its file";
                break;
            }
            key.gf = P->gmap[c->tu][x->file];
            key.begin = x->begin;
            key.end = x->end;
            key.text = x->text;
            key.id = fg->id;
            for (k = 0; k < gf->rd.len; k++) {
                size_t u = gf->rd.data[k];
                if (u == c->tu || !diag_map_enabled(P->R[u].enabled, fg->id))
                    continue;
                m++;
                nrep += reported(P, u, &key);
            }
            if (nrep < m)
                why = arena_printf(&P->arena, "not reported by %zu of %zu "
                                   "translation units that include %s", nrep, m,
                                   disp(P, gf->key));
        }
        if (why) {
            skip_cand(P, c, why, why_exit);
            continue;
        }
        for (e = 0; e < fg->ne; e++) {
            const FEdit *x = &r->edits.data[fg->e0 + e];
            const GFile *gf = &P->gf.data[P->gmap[c->tu][x->file]];
            CEdit ce;
            ce.gf = P->gmap[c->tu][x->file];
            ce.begin = x->begin;
            ce.end = x->end;
            ce.text = line_ending(P, gf, x->text);
            vec_push(&P->ce, ce);
        }
        c->e0 = (uint32_t)(P->ce.len - fg->ne);
        c->ne = fg->ne;
    }
    /* the same fix from several units: once */
    {
        uint32_t *perm = xmalloc((P->cands.len + 1) * sizeof *perm);
        size_t n = 0;
        for (t = 0; t < P->cands.len; t++)
            if (P->cands.data[t].state == CS_LIVE)
                perm[n++] = (uint32_t)t;
        sort_pass = P;
        sortn(perm, n, sizeof *perm, perm_cmp);
        for (t = 1; t < n; t++)
            if (!cand_cmp_sig(P, &P->cands.data[perm[t - 1]],
                              &P->cands.data[perm[t]]))
                P->cands.data[perm[t]].state = CS_DUP;
        free(perm);
    }
    /* conflicts: the builder, over the live candidates in order */
    {
        FixSet fs;
        VEC(uint32_t) num = {0};     /* builder group number -> candidate */
        size_t i;
        fixset_init(&fs);
        for (i = 0; i < P->gf.len; i++)
            fixset_file(&fs, P->gf.data[i].key);
        vec_push(&num, 0);
        for (t = 0; t < P->cands.len; t++) {
            const Cand *c = &P->cands.data[t];
            if (c->state != CS_LIVE)
                continue;
            fixset_group(&fs);
            for (e = 0; e < c->ne; e++) {
                const CEdit *x = &P->ce.data[c->e0 + e];
                fixset_add(&fs, x->gf, x->begin, x->end, x->text);
            }
            vec_push(&num, (uint32_t)t);
        }
        fixset_resolve(&fs);
        for (i = 1; i < num.len; i++) {
            uint32_t by = fs.skipped_by.data[i];
            const Cand *o;
            const FGroup *og;
            const FixResult *or;
            if (!by)
                continue;
            o = &P->cands.data[num.data[by < num.len ? by : i]];
            og = cgroup(P, o);
            or = &P->R[o->tu];
            skip_cand(P, &P->cands.data[num.data[i]],
                      arena_printf(&P->arena, "conflicts with an earlier fix "
                                   "at %s:%u:%u",
                                   og->pfile >= 0 ? disp(P, or->files.data[og->pfile].key) : "?",
                                   og->pline, og->pcol), true);
        }
        vec_free(&num);
        fixset_free(&fs);
    }
}

/* ---- the gate ---------------------------------------------------------------------- */

/* A trial: some candidates applied. */
typedef struct Sub {
    FixSet fs;
    char **etext;            /* per file: its new text, NULL if unedited */
    size_t *elen;
    size_t *lo;              /* per file: its accepted edits fs.acc[lo..lo+cnt) */
    size_t *cnt;
} Sub;

static void sub_build(Pass *P, Sub *s, const uint32_t *ids, size_t n)
{
    size_t i, e, nf = P->gf.len;
    memset(s, 0, sizeof *s);
    fixset_init(&s->fs);
    for (i = 0; i < nf; i++)
        fixset_file(&s->fs, P->gf.data[i].key);
    for (i = 0; i < n; i++) {
        const Cand *c = &P->cands.data[ids[i]];
        fixset_group(&s->fs);
        for (e = 0; e < c->ne; e++) {
            const CEdit *x = &P->ce.data[c->e0 + e];
            fixset_add(&s->fs, x->gf, x->begin, x->end, x->text);
        }
    }
    fixset_resolve(&s->fs);
    s->etext = xcalloc(nf, sizeof *s->etext);
    s->elen = xcalloc(nf, sizeof *s->elen);
    s->lo = xcalloc(nf, sizeof *s->lo);
    s->cnt = xcalloc(nf, sizeof *s->cnt);
    for (i = 0; i < s->fs.acc.len; i++) {
        uint32_t f = s->fs.acc.data[i].file;
        if (!s->cnt[f])
            s->lo[f] = i;
        s->cnt[f]++;
    }
    for (i = 0; i < nf; i++)
        if (s->cnt[i])
            s->etext[i] = fixset_apply(&s->fs, (uint32_t)i, P->gf.data[i].text,
                                       P->gf.data[i].len, &s->elen[i]);
}

static void sub_free(Sub *s)
{
    size_t i, nf = s->fs.files.len;
    for (i = 0; i < nf; i++)
        free(s->etext[i]);
    free(s->etext);
    free(s->elen);
    free(s->lo);
    free(s->cnt);
    fixset_free(&s->fs);
}

/* Where offset off of a file is after its edits; end: it ends a range, so
 * an insertion right there stays outside. */
static uint32_t map_off(const Sub *s, uint32_t f, uint32_t off, bool end)
{
    int64_t shift = 0;
    size_t i;
    for (i = 0; i < s->cnt[f]; i++) {
        const FixEdit *x = &s->fs.acc.data[s->lo[f] + i];
        bool ins = x->begin == x->end;
        if (x->begin > off)
            break;
        if (x->end <= off && !(end && ins && x->begin == off)) {
            shift += (int64_t)strlen(x->text) - (int64_t)(x->end - x->begin);
            continue;
        }
        if (x->begin == off && !ins)
            break;
        if (x->begin < off && off < x->end)
            return (uint32_t)((int64_t)x->begin + shift);
    }
    return (uint32_t)((int64_t)off + shift);
}

/* Is the edit among the accepted ones (the text as the file spells it)? */
static bool covered(Pass *P, const Sub *s, uint32_t f, const FEdit *x)
{
    size_t i;
    const char *text;
    if (!s->cnt[f])
        return false;
    text = line_ending(P, &P->gf.data[f], x->text);
    for (i = 0; i < s->cnt[f]; i++) {
        const FixEdit *a = &s->fs.acc.data[s->lo[f] + i];
        if (a->begin == x->begin && a->end == x->end && !strcmp(a->text, text))
            return true;
    }
    return false;
}

typedef struct DK {
    const char *file;
    uint32_t off;
    uint8_t level;
    const char *id, *msg;
    const FDiag *d;
    const FixResult *r;
} DK;

static int dk_cmp(const void *pa, const void *pb)
{
    const DK *a = pa, *b = pb;
    int c = strcmp(a->file, b->file);
    if (c)
        return c;
    if (a->off != b->off)
        return a->off < b->off ? -1 : 1;
    if (a->level != b->level)
        return a->level < b->level ? -1 : 1;
    c = strcmp(a->id, b->id);
    return c ? c : strcmp(a->msg, b->msg);
}

static void dk_of(DK *k, const FixResult *r, const FDiag *d, uint32_t off)
{
    k->file = d->file >= 0 ? r->files.data[d->file].path : "";
    k->off = off;
    k->level = d->level;
    k->id = d->id;
    k->msg = d->msg;
    k->d = d;
    k->r = r;
}

static const char *level_name(unsigned l)
{
    return l >= DL_ERROR ? "error" : l == DL_WARNING ? "warning" : "note";
}

static void describe(Pass *P, char *detail, const char *what, const DK *k)
{
    const FDiag *d = k->d;
    snprintf(detail, 240, "%s %s: %.80s [%s]%s%s", what, level_name(d->level),
             d->msg, *d->id ? d->id : "error",
             d->file >= 0 ? " at " : "",
             d->file >= 0 ? disp(P, k->r->files.data[d->file].key) : "");
    if (d->file >= 0) {
        size_t n = strlen(detail);
        snprintf(detail + n, 240 > n ? 240 - n : 0, ":%u:%u", d->line, d->col);
    }
}

static int rng_cmp(const void *pa, const void *pb)
{
    const FRange *a = pa, *b = pb;
    if (a->file != b->file)
        return a->file < b->file ? -1 : 1;
    if (a->begin != b->begin)
        return a->begin < b->begin ? -1 : 1;
    return a->end < b->end ? -1 : a->end > b->end;
}

/* The unit t before (r0) and after (r1): the gate's comparison. */
static bool compare_unit(Pass *P, const Sub *s, size_t t, const FixResult *r0,
                         const FixResult *r1, int mode, char *detail)
{
    size_t i, g, e, nfix = 0, nfixerr = 0, n0 = 0, n1 = 0, a, b;
    bool *fixed;
    DK *d0, *d1;
    bool ok = true;
    if (!r1->opened) {
        snprintf(detail, 240, "the unit can no longer be read");
        return false;
    }
    fixed = xcalloc(r0->diags.len + 1, sizeof *fixed);
    for (g = 0; g < r0->groups.len; g++) {
        const FGroup *fg = &r0->groups.data[g];
        bool all = fg->ne > 0;
        for (e = 0; e < fg->ne && all; e++) {
            const FEdit *x = &r0->edits.data[fg->e0 + e];
            all = covered(P, s, P->gmap[t][x->file], x);
        }
        if (all && !fixed[fg->diag]) {
            fixed[fg->diag] = true;
            nfix++;
            nfixerr += r0->diags.data[fg->diag].level >= DL_ERROR;
        }
    }
    /* a diagnostic that suggests the #include of a header an applied fix
     * adds falls with it, though it owns no fix-it (gcc gives only the first
     * note per header the edit) */
    for (g = 0; g < r0->groups.len; g++) {
        const FGroup *fg = &r0->groups.data[g];
        if (!fg->hdr || !fixed[fg->diag])
            continue;
        for (i = 0; i < r0->diags.len; i++)
            if (!fixed[i] && r0->diags.data[i].hdr &&
                !strcmp(r0->diags.data[i].hdr, fg->hdr)) {
                fixed[i] = true;
                nfix++;
                nfixerr += r0->diags.data[i].level >= DL_ERROR;
            }
    }
    d0 = xmalloc((r0->diags.len + 1) * sizeof *d0);
    d1 = xmalloc((r1->diags.len + 1) * sizeof *d1);
    for (i = 0; i < r1->diags.len; i++)
        dk_of(&d1[n1++], r1, &r1->diags.data[i], r1->diags.data[i].off);
    sortn(d1, n1, sizeof *d1, dk_cmp);
    if (mode == GM_LENIENT) {
        for (i = 0; i < r0->diags.len && ok; i++) {
            const FDiag *d = &r0->diags.data[i];
            DK k;
            if (!fixed[i])
                continue;
            dk_of(&k, r0, d, d->file >= 0
                                 ? map_off(s, P->gmap[t][d->file], d->off, false)
                                 : 0);
            if (bsearch(&k, d1, n1, sizeof *d1, dk_cmp)) {
                describe(P, detail, "leaves the", &k);
                ok = false;
            }
        }
        if (ok && r1->nerrors + nfixerr > r0->nerrors) {
            snprintf(detail, 240, "adds %u error%s", (unsigned)(r1->nerrors + nfixerr - r0->nerrors),
                     r1->nerrors + nfixerr - r0->nerrors == 1 ? "" : "s");
            ok = false;
        }
        goto out;
    }
    for (i = 0; i < r0->diags.len; i++) {
        const FDiag *d = &r0->diags.data[i];
        if (fixed[i])
            continue;
        dk_of(&d0[n0++], r0, d, d->file >= 0
                                    ? map_off(s, P->gmap[t][d->file], d->off, false)
                                    : 0);
    }
    sortn(d0, n0, sizeof *d0, dk_cmp);
    for (a = 0, b = 0; a < n0 || b < n1;) {
        int c = a >= n0 ? 1 : b >= n1 ? -1 : dk_cmp(&d0[a], &d1[b]);
        if (!c) {
            a++;
            b++;
            continue;
        }
        if (c < 0)
            describe(P, detail, "removes or changes the", &d0[a]);
        else
            describe(P, detail, "adds a", &d1[b]);
        ok = false;
        break;
    }
    if (ok) {                        /* the inactive #if groups */
        size_t m = r0->inactive.len;
        FRange *x0 = xmalloc((m + 1) * sizeof *x0);
        FRange *x1 = xmalloc((r1->inactive.len + 1) * sizeof *x1);
        for (i = 0; i < m; i++) {
            x0[i].file = (uint32_t)P->gmap[t][r0->inactive.data[i].file];
            x0[i].begin = map_off(s, x0[i].file, r0->inactive.data[i].begin, false);
            x0[i].end = map_off(s, x0[i].file, r0->inactive.data[i].end, true);
        }
        /* compared by the file's real path: the units name files by index */
        for (i = 0; i < r1->inactive.len; i++) {
            const char *key = r1->files.data[r1->inactive.data[i].file].key;
            uint32_t f = 0;
            while (f < P->gf.len && strcmp(P->gf.data[f].key, key))
                f++;
            x1[i].file = f;
            x1[i].begin = r1->inactive.data[i].begin;
            x1[i].end = r1->inactive.data[i].end;
        }
        sortn(x0, m, sizeof *x0, rng_cmp);
        sortn(x1, r1->inactive.len, sizeof *x1, rng_cmp);
        if (m != r1->inactive.len || (m && memcmp(x0, x1, m * sizeof *x0))) {
            snprintf(detail, 240, "changes which #if groups are inactive");
            ok = false;
        }
        free(x0);
        free(x1);
    }
    if (ok && mode == GM_PAREN && r0->digest != r1->digest) {
        snprintf(detail, 240, "changes the syntax tree");
        ok = false;
    }
out:
    (void)nfix;
    free(fixed);
    free(d0);
    free(d1);
    return ok;
}

/* Does applying ids (candidate indexes, ascending) pass the gate in `mode`?
 * detail gets the first difference. */
static bool gate_ok(Pass *P, int mode, const uint32_t *ids, size_t n, char *detail)
{
    Sub s;
    uint8_t *aff;
    FixResult *R1;
    FOv **ov;

    TuJob *jobs;
    size_t i, k, nj = 0;
    bool ok = true;
    detail[0] = 0;
    sub_build(P, &s, ids, n);
    aff = xcalloc(P->n + 1, 1);
    for (i = 0; i < P->gf.len; i++)
        if (s.cnt[i])
            for (k = 0; k < P->gf.data[i].rd.len; k++)
                aff[P->gf.data[i].rd.data[k]] = 1;
    for (i = 0; i < P->n; i++)
        nj += aff[i];
    R1 = xcalloc(nj + 1, sizeof *R1);
    jobs = xcalloc(nj + 1, sizeof *jobs);
    ov = xcalloc(nj + 1, sizeof *ov);

    for (i = 0, k = 0; i < P->n; i++) {
        size_t f, m = 0;
        FOv *mine = xcalloc(P->R[i].files.len + 1, sizeof *mine);
        if (!aff[i])
            continue;
        for (f = 0; f < P->R[i].files.len; f++) {
            uint32_t gi = P->gmap[i][f];
            if (!s.etext[gi])
                continue;
            mine[m].path = P->R[i].files.data[f].path;
            mine[m].buf = s.etext[gi];
            mine[m].len = s.elen[gi];
            m++;
        }
        ov[k] = mine;
        R1[k].ov = mine;
        R1[k].nov = m;
        R1[k].quiet = true;
        R1[k].base = P->base;
        R1[k].base_ctx = P->base_ctx;
        jobs[k] = P->jobs[i];
        jobs[k].res = &R1[k];
        jobs[k].fn = fix_tu;
        k++;
    }
    if (nj && P->base) {            /* the language server: stdout is its protocol */
        for (k = 0; k < nj; k++) {
            char *b = NULL;
            size_t bl = 0;
            FILE *e = open_memstream(&b, &bl);
            if (!e)
                fatal("out of memory");
            jobs[k].rc = jobs[k].fn(P->o, jobs[k].path, e, e, jobs[k].res);
            fclose(e);
            free(b);
        }
    } else if (nj)
        run_jobs(P->o, jobs, nj, stdout, false);
    for (i = 0, k = 0; i < P->n && ok; i++) {
        if (!aff[i])
            continue;
        ok = compare_unit(P, &s, i, &P->R[i], &R1[k], mode, detail);
        k++;
    }
    for (k = 0; k < nj; k++)
        fixres_free(&R1[k]);
    free(R1);
    free(jobs);
    for (k = 0; k < nj; k++)
        free(ov[k]);
    free(ov);

    free(aff);
    sub_free(&s);
    return ok;
}

/* ---- phases and bisection ------------------------------------------------------------ */

static int u32_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

typedef VEC(uint32_t) U32Vec;

/* The candidates L that pass on top of base and what was accepted so far
 * (acc, which grows), found by halving; the lone ones that fail go to fail. */
static void bisect(Pass *P, int mode, const U32Vec *base, U32Vec *acc,
                   const uint32_t *L, size_t n, U32Vec *fail)
{
    U32Vec test = {0};
    char detail[256];
    size_t i;
    if (!n)
        return;
    for (i = 0; i < base->len; i++)
        vec_push(&test, base->data[i]);
    for (i = 0; i < acc->len; i++)
        vec_push(&test, acc->data[i]);
    for (i = 0; i < n; i++)
        vec_push(&test, L[i]);
    sortn(test.data, test.len, sizeof *test.data, u32_cmp);
    if (gate_ok(P, mode, test.data, test.len, detail)) {
        for (i = 0; i < n; i++)
            vec_push(acc, L[i]);
    } else if (n == 1) {
        free(P->cands.data[L[0]].detail);
        P->cands.data[L[0]].detail = xstrdup(detail);
        vec_push(fail, L[0]);
    } else {
        size_t m = n / 2;
        bisect(P, mode, base, acc, L, m, fail);
        bisect(P, mode, base, acc, L + m, n - m, fail);
    }
    vec_free(&test);
}

/* The candidates that pass the gate, ascending. */
static void gate_all(Pass *P, U32Vec *out)
{
    U32Vec a1 = {0}, a2 = {0}, a3 = {0}, empty = {0}, base = {0}, L = {0},
           fail = {0};
    size_t i;
    for (i = 0; i < P->cands.len; i++)
        if (P->cands.data[i].state == CS_LIVE && P->cands.data[i].cls == 0)
            vec_push(&L, (uint32_t)i);
    bisect(P, GM_STRICT, &empty, &a1, L.data, L.len, &fail);
    L.len = 0;
    for (i = 0; i < P->cands.len; i++)
        if (P->cands.data[i].state == CS_LIVE && P->cands.data[i].cls == 1)
            vec_push(&L, (uint32_t)i);
    bisect(P, GM_PAREN, &empty, &a2, L.data, L.len, &fail);
    /* what failed the strict test may pass the lenient one, under --unsafe */
    L.len = 0;
    for (i = 0; i < P->cands.len; i++) {
        const Cand *c = &P->cands.data[i];
        bool failed = false;
        size_t k;
        for (k = 0; k < fail.len && !failed; k++)
            failed = fail.data[k] == i;
        if (c->state != CS_LIVE)
            continue;
        if (c->cls == 2 || (failed && P->fx->unsafe))
            vec_push(&L, (uint32_t)i);
        else if (failed)
            skip_cand(P, &P->cands.data[i],
                      arena_printf(&P->arena, "fails the semantic check: %s",
                                   c->detail), true);
    }
    for (i = 0; i < a1.len; i++)
        vec_push(&base, a1.data[i]);
    for (i = 0; i < a2.len; i++)
        vec_push(&base, a2.data[i]);
    sortn(base.data, base.len, sizeof *base.data, u32_cmp);
    fail.len = 0;
    bisect(P, GM_LENIENT, &base, &a3, L.data, L.len, &fail);
    for (i = 0; i < fail.len; i++) {
        Cand *c = &P->cands.data[fail.data[i]];
        skip_cand(P, c, arena_printf(&P->arena, "fails the semantic check: %s",
                                     c->detail), true);
    }
    for (i = 0; i < base.len; i++)
        vec_push(out, base.data[i]);
    for (i = 0; i < a3.len; i++)
        vec_push(out, a3.data[i]);
    sortn(out->data, out->len, sizeof *out->data, u32_cmp);
    vec_free(&a1);
    vec_free(&a2);
    vec_free(&a3);
    vec_free(&base);
    vec_free(&L);
    vec_free(&fail);
}

/* ---- the pass ---------------------------------------------------------------------------- */

typedef struct Totals {
    size_t edits, files;     /* written (or, with --diff, to be written) */
    VEC(char *) seen;        /* the files counted, by real path */
    VEC(char *) failed;      /* the files whose write error was printed */
    bool io_error;
    bool left;               /* something selected is still to be applied */
} Totals;

static int skip_cmp(const void *pa, const void *pb)
{
    const Skip *a = pa, *b = pb;
    int c = strcmp(a->path, b->path);
    if (c)
        return c;
    if (a->line != b->line)
        return a->line < b->line ? -1 : 1;
    if (a->col != b->col)
        return a->col < b->col ? -1 : 1;
    c = strcmp(a->id, b->id);
    return c ? c : strcmp(a->why, b->why);
}

static void drop_skips(Pass *P)
{
    size_t i;
    for (i = 0; i < P->skips.len; i++) {
        free(P->skips.data[i].path);
        free(P->skips.data[i].id);
        free(P->skips.data[i].why);
    }
    P->skips.len = 0;
}

/* Re-reads each file to be written; if one is not what the units saw, its
 * candidates are dropped from ids.  True if some were. */
static bool recheck(Pass *P, const Sub *s, U32Vec *ids)
{
    bool dropped = false;
    size_t f, i, e;
    const char *hook = getenv("CEREAL_FIX_HOOK");  /* tests: an editor saves */
    if (hook && *hook && system(hook) != 0)
        fprintf(stderr, "cereal: fix: CEREAL_FIX_HOOK failed\n");
    for (f = 0; f < P->gf.len; f++) {
        const GFile *g = &P->gf.data[f];
        char *raw;
        size_t n, off = 0;
        bool same;
        if (!s->cnt[f])
            continue;
        raw = read_file(g->key, &n);
        if (raw && n >= 3 && !memcmp(raw, "\357\273\277", 3))
            off = 3;
        same = raw && fnv(raw + off, n - off) == g->hash;
        if (!raw)
            P->io_error = true;
        free(raw);
        if (same)
            continue;
        dropped = true;
        for (i = 0; i < ids->len; i++) {
            Cand *c = &P->cands.data[ids->data[i]];
            bool touches = false;
            for (e = 0; e < c->ne; e++)
                touches |= P->ce.data[c->e0 + e].gf == f;
            if (touches) {
                skip_cand(P, c, raw ? "changed during the run"
                                    : "cannot be read again", true);
                ids->data[i] = UINT32_MAX;
            }
        }
    }
    if (dropped) {
        size_t k = 0;
        for (i = 0; i < ids->len; i++)
            if (ids->data[i] != UINT32_MAX)
                ids->data[k++] = ids->data[i];
        ids->len = k;
    }
    return dropped;
}

/* One pass over P->R: select, gate, then diff or write.  tot gets what was
 * done; true if some file was changed. */
static bool merge_pass(Pass *P, Totals *tot)
{
    U32Vec ids = {0};
    Sub s;
    size_t f;
    bool wrote = false, nowrite = P->fx->diff || P->fx->dry || P->probe;
    P->gmap =xcalloc(P->n + 1, sizeof *P->gmap);
    P->eidx = xcalloc(P->n + 1, sizeof *P->eidx);
    P->neidx = xcalloc(P->n + 1, sizeof *P->neidx);
    build_table(P);
    select_cands(P);
    gate_all(P, &ids);
    if (!ids.len)
        goto done;
    sub_build(P, &s, ids.data, ids.len);
    if (!nowrite && recheck(P, &s, &ids)) {
        sub_free(&s);
        sub_build(P, &s, ids.data, ids.len);
    }
    for (f = 0; f < P->gf.len; f++) {
        const GFile *g = &P->gf.data[f];
        size_t nacc = s.cnt[f];
        if (!nacc)
            continue;
        if (P->probe) {
            tot->left = true;
            continue;
        }
        if (P->fx->diff) {
            if (!fixset_diff(&s.fs, (uint32_t)f, disp(P, g->key), g->text,
                             g->len, stdout)) {
                fprintf(stderr, "cereal: error: %s: an edit crosses a line\n",
                        disp(P, g->key));
                tot->io_error = true;
            }
        } else if (!P->fx->dry) {
            StrBuf sb = {0};
            char err[512];
            if (g->bom)
                sb_putn(&sb, "\357\273\277", 3);
            sb_putn(&sb, s.etext[f], s.elen[f]);
            if (!file_write_atomic(g->key, sb.data ? sb.data : "", sb.len, err,
                                   sizeof err)) {
                size_t k;       /* each later pass retries the file: say it once */
                for (k = 0; k < tot->failed.len; k++)
                    if (!strcmp(tot->failed.data[k], g->key))
                        break;
                if (k == tot->failed.len) {
                    fprintf(stderr, "cereal: error: %s\n", err);
                    vec_push(&tot->failed, xstrdup(g->key));
                }
                tot->io_error = true;
                sb_free(&sb);
                continue;
            }
            sb_free(&sb);
            wrote = true;
        }
        tot->edits += nacc;
        {
            size_t k;
            for (k = 0; k < tot->seen.len; k++)
                if (!strcmp(tot->seen.data[k], g->key))
                    break;
            if (k == tot->seen.len) {
                vec_push(&tot->seen, xstrdup(g->key));
                tot->files++;
            }
        }
        tot->left = true;
    }
    sub_free(&s);
done:
    vec_free(&ids);
    return wrote;
}

static void pass_free(Pass *P)
{
    size_t i;
    for (i = 0; i < P->gf.len; i++) {
        free(P->gf.data[i].text);
        vec_free(&P->gf.data[i].rd);
    }
    vec_free(&P->gf);
    for (i = 0; i < P->n; i++) {
        free(P->gmap ? P->gmap[i] : NULL);
        free(P->eidx ? P->eidx[i] : NULL);
    }
    free(P->gmap);
    free(P->eidx);
    free(P->neidx);
    for (i = 0; i < P->cands.len; i++)
        free(P->cands.data[i].detail);
    vec_free(&P->cands);
    vec_free(&P->ce);
    arena_free(&P->arena);
    P->gmap = NULL;
    P->eidx = NULL;
    P->neidx = NULL;
}

/* ---- fix_run ------------------------------------------------------------------------------- */

int fix_run(Options *o, const char *ccdb, const StrVec *own, const FixOpts *fxin)
{
    FixOpts fx = *fxin;
    CdbPlan plan;
    TuJob *jobs = NULL;
    size_t n = 0, i;
    FixResult *R;
    Pass P;
    Totals tot;
    char cwd[PATH_MAX], root[PATH_MAX];
    int rc = 0, pass, wpasses = 0;
    bool planned = false, unopened = false, remains = false;
    if (fx.edits)
        return fx.diff && !fx.dry && !fx.apply ? edits_diff(fx.edits)
               : (fputs("cereal: fix --edits=FILE needs --diff\n", stderr), 2);
    if ((fx.apply && (fx.diff || fx.dry)) || (fx.diff && fx.dry)) {
        fputs("cereal: --apply, --diff and --dry-run exclude each other\n", stderr);
        return 2;
    }
    if (fx.only) {
        char id[128];
        if (unknown_only_id(fx.only, id, sizeof id)) {
            fprintf(stderr, "cereal: --only: '%s' is not the id of a diagnostic "
                    "with a fix-it\n", id);
            return 2;
        }
    }
    if (!fx.max_passes)
        fx.max_passes = 8;
    if (!getcwd(cwd, sizeof cwd))
        strcpy(cwd, ".");
    if (!realpath(fx.root ? fx.root : cwd, root)) {
        fprintf(stderr, "cereal: --root: cannot resolve '%s'\n",
                fx.root ? fx.root : cwd);
        return 2;
    }
    if (ccdb) {
        rc = compdb_plan(o, fix_tu, ccdb, own, &plan);
        if (rc)
            return rc;
        planned = true;
        jobs = plan.jobs;
        n = plan.njobs;
        fprintf(stderr, "cereal: %zu entries: %zu checked, %zu not C, %zu "
                "duplicate, %zu not checked\n", plan.nsel, plan.njobs,
                plan.n_notc, plan.n_dup, plan.n_not);
    } else {
        if (!o->inputs.len) {
            fputs("cereal: fix needs input files\n", stderr);
            return 2;
        }
        n = o->inputs.len;
        jobs = xcalloc(n, sizeof *jobs);
        for (i = 0; i < n; i++) {
            jobs[i].fn = fix_tu;
            jobs[i].path = o->inputs.data[i];
        }
    }
    R = xcalloc(n + 1, sizeof *R);
    memset(&tot, 0, sizeof tot);
    memset(&P, 0, sizeof P);
    P.o = o;
    P.fx = &fx;
    P.jobs = jobs;
    P.n = n;
    P.R = R;
    P.cwd = cwd;
    P.root = root;
    for (pass = 1;; pass++) {
        bool wrote;
        P.probe = pass > fx.max_passes;
        for (i = 0; i < n; i++) {
            fixres_free(&R[i]);
            R[i].quiet = pass > 1;
            jobs[i].fn = fix_tu;
            jobs[i].res = &R[i];
        }
        run_jobs(o, jobs, n, stdout, false);
        unopened = false;
        for (i = 0; i < n; i++)
            unopened |= !R[i].opened;
        arena_init(&P.arena);
        drop_skips(&P);
        tot.left = false;
        wrote = merge_pass(&P, &tot);
        wpasses += wrote || fx.diff || fx.dry;
        remains = P.probe && tot.left;
        pass_free(&P);
        if (P.probe || fx.diff || fx.dry || !wrote)
            break;
    }
    /* the report */
    {
        size_t k, nskip = 0;
        bool exit_skip = false;
        sortn(P.skips.data, P.skips.len, sizeof *P.skips.data, skip_cmp);
        for (k = 0; k < P.skips.len; k++) {
            const Skip *s = &P.skips.data[k];
            fprintf(stderr, "cereal fix: %s:%u:%u: skipped [%s]: %s\n", s->path,
                    s->line, s->col, s->id, s->why);
            nskip++;
            exit_skip |= s->exit_flag;
        }
        if (remains) {
            fprintf(stderr, "cereal fix: fixes remain after %d passes\n",
                    fx.max_passes);
            exit_skip = true;
        }
        if (tot.edits || nskip)
            fprintf(stderr, "cereal fix: %s%zu edit%s in %zu file%s in %d pass%s;"
                    " %zu skipped\n",
                    fx.dry || fx.diff ? "would apply " : "",
                    tot.edits, tot.edits == 1 ? "" : "s", tot.files,
                    tot.files == 1 ? "" : "s", wpasses,
                    wpasses == 1 ? "" : "es", nskip);
        rc = 0;
        if (exit_skip || ((fx.diff || fx.dry) && tot.edits))
            rc = 1;
        if (unopened || (planned && plan.n_not))
            rc = 3;
        if (tot.io_error || P.io_error)
            rc = 4;
    }
    drop_skips(&P);
    vec_free(&P.skips);
    for (i = 0; i < tot.seen.len; i++)
        free(tot.seen.data[i]);
    vec_free(&tot.seen);
    for (i = 0; i < tot.failed.len; i++)
        free(tot.failed.data[i]);
    vec_free(&tot.failed);
    for (i = 0; i < n; i++)
        fixres_free(&R[i]);
    free(R);
    if (planned)
        compdb_plan_free(&plan);
    else
        free(jobs);
    return rc;
}

/* ---- the language server's fix-all (docs/C_DESIGN.md 7) ------------------------------ */

void fix_edits_free(FixTextEdits *e)
{
    size_t i;
    for (i = 0; i < e->len; i++)
        free(e->data[i].text);
    vec_free(e);
    memset(e, 0, sizeof *e);
}

bool fix_unit_edits(Options *o, const char *main, const char *file,
                    FixBaseFn base, void *base_ctx, FixTextEdits *out)
{
    FixOpts fx;
    FixResult R;
    TuJob job;
    Pass P;
    U32Vec ids = {0};
    char cwd[PATH_MAX];
    size_t i, f;
    bool opened;
    memset(&fx, 0, sizeof fx);
    memset(&R, 0, sizeof R);
    memset(&P, 0, sizeof P);
    memset(&job, 0, sizeof job);
    if (!getcwd(cwd, sizeof cwd))
        strcpy(cwd, ".");
    R.quiet = true;
    R.base = base;
    R.base_ctx = base_ctx;
    job.o = o;
    job.fn = fix_tu;
    job.path = main;
    job.res = &R;
    {
        char *b = NULL;
        size_t bl = 0;
        FILE *e = open_memstream(&b, &bl);
        if (!e)
            fatal("out of memory");
        fix_tu(o, main, e, e, &R);
        fclose(e);
        free(b);
    }
    opened = R.opened;
    if (!opened) {
        fixres_free(&R);
        return false;
    }
    P.o = o;
    P.fx = &fx;
    P.jobs = &job;
    P.n = 1;
    P.R = &R;
    P.cwd = cwd;
    P.root = "/";
    P.base = base;
    P.base_ctx = base_ctx;
    P.only_path = file;
    arena_init(&P.arena);
    P.gmap = xcalloc(2, sizeof *P.gmap);
    P.eidx = xcalloc(2, sizeof *P.eidx);
    P.neidx = xcalloc(2, sizeof *P.neidx);
    build_table(&P);
    select_cands(&P);
    gate_all(&P, &ids);
    if (ids.len) {
        Sub s;
        sub_build(&P, &s, ids.data, ids.len);
        for (f = 0; f < P.gf.len; f++)
            if (P.gf.data[f].path && !strcmp(P.gf.data[f].path, file))
                for (i = 0; i < s.cnt[f]; i++) {
                    const FixEdit *x = &s.fs.acc.data[s.lo[f] + i];
                    FixTextEdit t;
                    t.begin = x->begin;
                    t.end = x->end;
                    t.text = xstrdup(x->text);
                    vec_push(out, t);
                }
        sub_free(&s);
    }
    vec_free(&ids);
    drop_skips(&P);
    vec_free(&P.skips);
    pass_free(&P);
    fixres_free(&R);
    return true;
}
