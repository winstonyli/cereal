/* crename.c - renaming a C name (B2 phase 4; docs/B2_DESIGN.md, "Phase 4
 * design addendum"; B5_DESIGN.md 11.5).  The edits are one list per file:
 * given by the caller (the project's, for a name used in several units) or
 * planned on a check of the unit as it is (cindex_rename_plan).  The unit is
 * then checked again with them applied, over an overlay of every file it
 * reads that has edits.  The rename stands only if the two runs give the same events,
 * decls and diagnostics, up to the moved offsets and the new name.  That
 * comparison is the conflict check: the checker's own name lookup, for
 * every scope and namespace, rather than an imitation of it. */
#include "c/csymidx.h"
#include "c/frontend.h"
#include "pp.h"

#include <string.h>

typedef struct EdText {      /* a file with the edits applied */
    const CRenFile *f;
    char *text;
    size_t len;
} EdText;

typedef struct Run {
    TU tu;
    CIndex *ix;
    VEC(SrcRange) skipped;   /* inactive #if groups */
    char *missing;           /* the first #include not found */
    const CRename *q;
    bool live;
    char fatal[256];         /* run_check: the fatal error that ended it, else "" */
    const EdText *ed;       /* the second run: the files' edited texts */
    size_t ned;
} Run;

static bool run_overlay(void *ctx, const char *path, const char **buf,
                        size_t *len)
{
    Run *r = ctx;
    size_t i;
    for (i = 0; i < r->ned; i++)
        if (!strcmp(path, r->ed[i].f->path)) {
            *buf = r->ed[i].text;
            *len = r->ed[i].len;
            return true;
        }
    return r->q->overlay && r->q->overlay(r->q->overlay_ctx, path, buf, len);
}

static void on_skipped(void *ctx, SrcLoc b, SrcLoc e)
{
    Run *r = ctx;
    SrcRange x;
    x.begin = b;
    x.end = e;
    vec_push(&r->skipped, x);
}

static void on_include(void *ctx, const IncludeEvent *ev)
{
    Run *r = ctx;
    if (ev->result == INC_NOT_FOUND && !r->missing)
        r->missing = xstrdup(ev->spelled ? ev->spelled : "?");
}

/* Checks the unit into r, with its symbol index; false if it could not (a fatal
 * error in the check is that too, its text in r->fatal; r is then freed by run_free
 * like any other, not lost with the abandoned frames). */
static bool run_check(Run *r, const CRename *q)
{
    PPListener l;
    FrontendOpts fo;
    FatalTrap tr;
    bool ok;
    r->q = q;
    fatal_trap_push(&tr);
    if (setjmp(tr.jb)) {
        snprintf(r->fatal, sizeof r->fatal, "%s", tr.msg);
        return false;
    }
    r->live = true;
    tu_init(&r->tu, q->o);
    r->tu.sm.overlay = run_overlay;
    r->tu.sm.overlay_ctx = r;
    memset(&l, 0, sizeof l);
    l.ctx = r;
    l.skipped = on_skipped;
    l.include = on_include;
    pp_add_listener(&r->tu.pp, l);
    memset(&fo, 0, sizeof fo);
    fo.check = true;
    fo.cidx = &r->ix;
    ok = frontend_run(&r->tu, q->main, &fo) && r->ix;
    fatal_trap_pop(&tr);
    return ok;
}

static void run_free(Run *r)
{
    if (!r->live)
        return;
    cindex_free(r->ix);
    vec_free(&r->skipped);
    free(r->missing);
    tu_free(&r->tu);
}

/* ---- the mapping between the two texts of the main file ------------------- */

/* The edit list of a file (NULL: the file has none). */
static const CRenFile *file_edits(const CRename *q, const char *path)
{
    size_t i;
    for (i = 0; i < q->nfiles; i++)
        if (!strcmp(q->files[i].path, path))
            return &q->files[i];
    return NULL;
}

/* Where offset off of a file is after its edits f (NULL: none); *edited: an
 * edit starts there. */
static uint32_t map_off(const CRenFile *f, const CRename *q, uint32_t nl,
                        uint32_t off, bool *edited)
{
    size_t lo = 0, hi = f ? f->n : 0;
    *edited = false;
    if (!f)
        return off;
    while (lo < hi) {        /* the edits before off */
        size_t mid = (lo + hi) / 2;
        if (f->offs[mid] < off)
            lo = mid + 1;
        else
            hi = mid;
    }
    *edited = lo < f->n && f->offs[lo] == off;
    return off + (uint32_t)lo * nl - (uint32_t)lo * q->len;
}

/* The original offset of offset y of the edited text of a file (an edit's
 * start for a place inside the new name). */
static uint32_t unmap_off(const CRenFile *f, const CRename *q, uint32_t nl,
                          uint32_t y)
{
    uint32_t k;
    if (!f)
        return y;
    for (k = 0; k < f->n; k++) {
        uint32_t p = f->offs[k] + k * nl - k * q->len;
        if (y < p)
            break;
        if (y < p + nl)
            return f->offs[k];
    }
    return y - k * nl + k * q->len;
}

/* ---- the blockers that need the new name or the unit (A3.6-A3.9) -------- */

static bool word_at(const char *p, size_t n, const char *w)
{
    size_t l = strlen(w);
    return l <= n && !memcmp(p, w, l) && (l == n || !cindex_ident_char(p[l]));
}

/* The old or the new name as a word in an inactive #if group of a user file,
 * within the scope of decl d (A3.9). */
static bool inactive_use(Run *a, uint32_t d, const char *old, const char *nw,
                         StrBuf *why, CRenInactive *inact)
{
    /* d: UINT32_MAX when the unit has no event at an edit: the whole unit */
    const CIdxDecl *x = d == UINT32_MAX ? NULL : &a->ix->decls[d];
    uint32_t root = x ? cindex_scope_root(a->ix, x->scope) : 0;   /* its external declaration */
    const CIdxScope *s = root ? &a->ix->scopes[root - 1] : NULL;
    size_t i;
    for (i = 0; i < a->skipped.len; i++) {
        SrcRange r = a->skipped.data[i];
        SrcFile *f = srcmgr_file_of(&a->tu.sm, r.begin);
        uint32_t b, e, k;
        if (!f || f->kind != SF_USER || f->system_header)
            continue;
        b = r.begin - f->base;
        e = r.end - f->base > f->size ? f->size : r.end - f->base;
        if (s) {
            if (strcmp(f->path, a->ix->files[s->file].path))
                continue;
            b = b < s->begin ? s->begin : b;
            e = e > s->end ? s->end : e;
        }
        for (k = b; k < e; k++) {
            const char *w = word_at(f->buf + k, e - k, old) ? old
                          : word_at(f->buf + k, e - k, nw)  ? nw
                                                            : NULL;
            char at[512];
            if (!w || (k > 0 && cindex_ident_char(f->buf[k - 1])))
                continue;
            if (inact) {
                StrBuf m = {0};
                CRenHit *h;
                sb_printf(&m, "'%s' occurs in an inactive #if group at %s, which "
                          "cannot be checked", w,
                          cindex_place(&a->tu.sm, f->path, k, at, sizeof at));
                inact->hits = xrealloc(inact->hits, (inact->nhits + 1) * sizeof *inact->hits);
                h = &inact->hits[inact->nhits++];
                h->path = xstrdup(f->path);
                h->off = k;
                h->msg = (char *)sb_cstr(&m);
                k += strlen(w) - 1;
                continue;
            }
            sb_printf(why, "'%s' occurs in an inactive #if group at %s, which "
                      "cannot be checked", w,
                      cindex_place(&a->tu.sm, f->path, k, at, sizeof at));
            return true;
        }
    }
    return false;
}

void c_rename_inactive_free(CRenInactive *x)
{
    size_t i;
    for (i = 0; i < x->nhits; i++) {
        free(x->hits[i].path);
        free(x->hits[i].msg);
    }
    for (i = 0; i < x->nreads; i++)
        free(x->reads[i]);
    free(x->hits);
    free(x->reads);
    memset(x, 0, sizeof *x);
}

static bool has_hit(const CRenInactive *x, const CRenHit *h)
{
    size_t i;
    for (i = 0; i < x->nhits; i++)
        if (x->hits[i].off == h->off && !strcmp(x->hits[i].path, h->path))
            return true;
    return false;
}

static bool reads_file(const CRenInactive *x, const char *path)
{
    size_t i;
    for (i = 0; i < x->nreads; i++)
        if (!strcmp(x->reads[i], path))
            return true;
    return false;
}

char *c_rename_uncovered(const CRenInactive *unit, const CRenInactive *const *others,
                         size_t n)
{
    size_t i, j;
    for (i = 0; i < unit->nhits; i++) {
        const CRenHit *h = &unit->hits[i];
        for (j = 0; j < n; j++)
            if (reads_file(others[j], h->path) && !has_hit(others[j], h))
                break;
        if (j == n)
            return xstrdup(h->msg);
    }
    return NULL;
}

/* ---- the comparison (A1) ------------------------------------------------------ */

typedef struct Cmp {
    Run *a, *b;              /* the original run, the edited one */
    const CRename *q;
    uint32_t nl;             /* the new name's length */
    const CRenFile **fe;     /* the edits of each file of a's index (the same
                                entries in b's, once checked) */
    bool *ren;               /* the decls of a's index with an event at an edit */
    char p1[512], p2[512];
} Cmp;

/* The place in the original text of an event of a's index or (second) of
 * b's. */
static const char *ev_place(Cmp *c, const CIdxEvent *e, bool second, char *buf)
{
    uint32_t off = second ? unmap_off(c->fe[e->file], c->q, c->nl, e->off) : e->off;
    return cindex_place(&c->a->tu.sm, c->a->ix->files[e->file].path, off, buf,
                        sizeof c->p1);
}

/* "KIND 'NAME' declared at PLACE" for decl d of a's index or (second) b's. */
static void describe(Cmp *c, StrBuf *sb, uint32_t d, bool second)
{
    const CIndex *ix = second ? c->b->ix : c->a->ix;
    uint32_t j;
    sb_printf(sb, "%s '%s'", cindex_kind_name(ix->decls[d].kind), cindex_name(ix, d));
    for (j = ix->by_decl_start[d]; j < ix->by_decl_start[d + 1]; j++) {
        const CIdxEvent *e = &ix->ev[ix->by_decl[j]];
        if ((e->flags & CIX_ROLE) != CIX_REF) {
            sb_printf(sb, " declared at %s", ev_place(c, e, second, c->p2));
            return;
        }
    }
}

/* The places of a diagnostic in the two runs agree (main-file offsets moved
 * by the edits; a scratch buffer's are not compared). */
static bool same_diag_place(Cmp *c, SrcLoc la, SrcLoc lb)
{
    SrcFile *fa = la ? srcmgr_file_of(&c->a->tu.sm, la) : NULL;
    SrcFile *fb = lb ? srcmgr_file_of(&c->b->tu.sm, lb) : NULL;
    uint32_t off;
    bool ed;
    if (!fa || !fb)
        return !fa && !fb;
    if (fa->id != fb->id)
        return false;
    if (fa->kind != SF_USER && fa->kind != SF_SYSTEM)
        return true;
    off = map_off(file_edits(c->q, fa->path), c->q, c->nl, la - fa->base, &ed);
    return off == lb - fb->base;
}

static const char *diag_place(Cmp *c, Run *r, SrcLoc loc)
{
    SrcFile *f = loc ? srcmgr_file_of(&r->tu.sm, loc) : NULL;
    uint32_t off;
    if (!f)
        return "?";
    off = loc - f->base;
    if (r == c->b)
        off = unmap_off(file_edits(c->q, f->path), c->q, c->nl, off);
    return cindex_place(&c->a->tu.sm, f->path, off, c->p1, sizeof c->p1);
}

/* Appends to why how the edited run differs from the original; false if it
 * does not (c->ren: the renamed decls).  Diagnostics first: they say best what
 * went wrong (a redeclaration, a duplicate member); a capture has none. */
static bool differs(Cmp *c, StrBuf *why)
{
    const CIndex *x = c->a->ix, *y = c->b->ix;
    const DiagEngine *ga = &c->a->tu.diag, *gb = &c->b->tu.diag;
    uint32_t i, k, nlen = c->nl > 255 ? 255 : c->nl;
    bool same = x->nfiles == y->nfiles && x->ndecls == y->ndecls;
    for (k = 0; k < ga->all.len || k < gb->all.len; k++) {
        const Diagnostic *g = k < ga->all.len ? ga->all.data[k] : NULL;
        const Diagnostic *h = k < gb->all.len ? gb->all.data[k] : NULL;
        if (g && h && g->level == h->level && !strcmp(g->id, h->id) &&
            same_diag_place(c, g->loc, h->loc))
            continue;
        if (h)
            sb_printf(why, "renaming would cause a diagnostic at %s: %s",
                      diag_place(c, c->b, h->loc), h->msg);
        else
            sb_printf(why, "renaming would remove the diagnostic at %s: %s",
                      diag_place(c, c->a, g->loc), g->msg);
        return true;
    }
    for (i = 0; same && i < x->nfiles; i++)
        same = !strcmp(x->files[i].path, y->files[i].path);
    for (i = 0; same && i < x->ndecls; i++)
        same = x->decls[i].kind == y->decls[i].kind &&
               x->decls[i].linkage == y->decls[i].linkage;
    if (!same) {
        sb_puts(why, "renaming would change the declarations of the unit");
        return true;
    }
    for (i = 0; i < x->ndecls; i++) {
        const char *want = c->ren[i] ? c->q->name : cindex_name(x, i);
        if (!strcmp(cindex_name(y, i), want))
            continue;
        sb_puts(why, "a renamed name also names the ");
        describe(c, why, i, false);
        sb_puts(why, ", which would be renamed too");
        return true;
    }
    for (k = 0; k < x->nev || k < y->nev; k++) {
        const CIdxEvent *e = k < x->nev ? &x->ev[k] : NULL;
        const CIdxEvent *f = k < y->nev ? &y->ev[k] : NULL;
        uint32_t off = 0;
        bool ed = false;
        if (e)
            off = map_off(c->fe[e->file], c->q, c->nl, e->off, &ed);
        if (e && f && e->file == f->file && off == f->off) {
            if (e->decl == f->decl && e->flags == f->flags &&
                f->len == (ed ? nlen : e->len))
                continue;
            sb_printf(why, "after renaming, the name at %s would refer to the ",
                      ev_place(c, e, false, c->p1));
            describe(c, why, f->decl, true);
            sb_puts(why, " instead of the ");
            describe(c, why, e->decl, false);
        } else if (e && (!f || e->file < f->file ||
                         (e->file == f->file && off < f->off))) {
            sb_printf(why, "after renaming, the name at %s would no longer refer "
                      "to the ", ev_place(c, e, false, c->p1));
            describe(c, why, e->decl, false);
        } else {
            sb_printf(why, "after renaming, the name at %s would refer to the ",
                      ev_place(c, f, true, c->p1));
            describe(c, why, f->decl, true);
        }
        return true;
    }
    return false;
}

/* Plans the edits from the unit's own index: every use of the name at
 * q->path, q->off, in the main file (the others need the project). */
static bool plan_unit(CRename *q, Run *a, StrBuf *why)
{
    char msg[1024];
    SrcFile *mf = cindex_srcfile(&a->tu.sm, path_normalize(&a->tu.arena, q->main));
    uint32_t *ev = NULL;
    size_t n = 0, k;
    const char *err;
    CRenFile *f;
    if (!mf) {
        sb_puts(why, "the unit could not be checked");
        return false;
    }
    if ((err = cindex_rename_plan(a->ix, &a->tu.sm, q->path, q->off, mf->path,
                                  false, &ev, &n, msg, sizeof msg)) != NULL) {
        sb_puts(why, err);
        return false;
    }
    f = xcalloc(1, sizeof *f);
    f->path = xstrdup(mf->path);
    f->offs = xmalloc((n + 1) * sizeof *f->offs);
    f->n = n;
    for (k = 0; k < n; k++)
        f->offs[k] = a->ix->ev[ev[k]].off;
    q->old = xstrdup(cindex_name(a->ix, a->ix->ev[ev[0]].decl));
    q->files = f;
    q->nfiles = 1;
    q->own = true;
    free(ev);
    return true;
}

static bool has_off(const CRenFile *f, uint32_t off)
{
    size_t lo = 0, hi = f->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (f->offs[mid] < off)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < f->n && f->offs[lo] == off;
}

char *c_rename(CRename *q)
{
    Run a, b;
    Cmp c;
    StrBuf why = {0};
    EdText *ed = NULL;
    const CRenFile **fe = NULL;
    bool *ren = NULL;
    uint32_t first = UINT32_MAX;
    size_t ned = 0, i, k;
    Ident *id;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    memset(&c, 0, sizeof c);
    q->len = 0;
    if (!run_check(&a, q)) {
        sb_printf(&why, "the unit could not be checked%s%s", a.fatal[0] ? ": " : "", a.fatal);
        goto out;
    }
    if (!q->files && !plan_unit(q, &a, &why))
        goto out;
    q->len = (uint32_t)strlen(q->old);
    fe = xcalloc(a.ix->nfiles + 1, sizeof *fe);
    ren = xcalloc(a.ix->ndecls + 1, sizeof *ren);
    for (i = 0; i < a.ix->nfiles; i++)
        fe[i] = file_edits(q, a.ix->files[i].path);
    /* The renamed decl is the one at the asked place, else the first an edit names;
     * another decl with an event at an edit (a macro argument that is also a
     * declaration) is renamed by accident, which differs() refuses. */
    for (k = 0; k < a.ix->nev; k++) {
        const CIdxEvent *e = &a.ix->ev[k];
        if (!fe[e->file] || !has_off(fe[e->file], e->off))
            continue;
        if (first == UINT32_MAX)
            first = e->decl;
        if (q->path && e->off == q->off && !strcmp(a.ix->files[e->file].path, q->path)) {
            first = e->decl;
            break;
        }
    }
    if (first != UINT32_MAX)
        ren[first] = true;
    id = intern_find(a.tu.in, q->name, strlen(q->name));
    if (!cindex_is_identifier(q->name))
        sb_printf(&why, "'%s' is not an identifier", q->name);
    else if (id && id->ckw)
        sb_printf(&why, "'%s' is a keyword", q->name);
    else if (id && mt_hist(a.tu.pp.mt, id))
        sb_printf(&why, "'%s' is a macro name in this unit", q->name);
    else if (a.missing)
        sb_printf(&why, "#include %s was not found: uses of '%s' there cannot "
                  "be seen", a.missing, q->old);
    else
        inactive_use(&a, first, q->old, q->name, &why, q->inact);
    if (q->inact) {
        for (i = 0; i < a.ix->nfiles; i++) {
            q->inact->reads = xrealloc(q->inact->reads, (q->inact->nreads + 1) * sizeof *q->inact->reads);
            q->inact->reads[q->inact->nreads++] = xstrdup(a.ix->files[i].path);
        }
    }
    if (why.len)
        goto out;
    /* the edited files the unit reads, then the comparison */
    ed = xcalloc(q->nfiles + 1, sizeof *ed);
    for (i = 0; i < q->nfiles; i++) {
        const CRenFile *f = &q->files[i];
        SrcFile *sf = cindex_srcfile(&a.tu.sm, f->path);
        StrBuf t = {0};
        uint32_t last = 0;
        if (!sf)
            continue;
        for (k = 0; k < f->n; k++) {
            uint32_t o = f->offs[k];
            char at[512];
            if (o < last || (size_t)o + q->len > sf->size ||
                memcmp(sf->buf + o, q->old, q->len)) {
                sb_printf(&why, "the text at %s is not '%s': the file changed",
                          cindex_place(&a.tu.sm, f->path, o < sf->size ? o : 0, at,
                                       sizeof at), q->old);
                sb_free(&t);
                goto out;
            }
            sb_putn(&t, sf->buf + last, o - last);
            sb_puts(&t, q->name);
            last = o + q->len;
        }
        sb_putn(&t, sf->buf + last, sf->size - last);
        ed[ned].f = f;
        ed[ned].len = t.len;
        ed[ned++].text = (char *)sb_cstr(&t);
    }
    b.ed = ed;
    b.ned = ned;
    if (!run_check(&b, q)) {
        sb_printf(&why, "the renamed unit could not be checked%s%s", b.fatal[0] ? ": " : "",
                  b.fatal);
        goto out;
    }
    c.a = &a;
    c.b = &b;
    c.q = q;
    c.nl = (uint32_t)strlen(q->name);
    c.fe = fe;
    c.ren = ren;
    differs(&c, &why);
out:
    run_free(&b);
    run_free(&a);
    for (i = 0; i < ned; i++)
        free(ed[i].text);
    free(ed);
    free(fe);
    free(ren);
    if (!why.len)
        return NULL;
    sb_cstr(&why);
    return why.data;
}

void c_rename_free(CRename *q)
{
    size_t i;
    if (!q->own)
        return;
    for (i = 0; i < q->nfiles; i++) {
        free((char *)q->files[i].path);
        free(q->files[i].offs);
    }
    free(q->files);
    free((char *)q->old);
    q->files = NULL;
    q->old = NULL;
    q->nfiles = 0;
    q->own = false;
}
