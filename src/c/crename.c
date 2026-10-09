/* crename.c - renaming a C name (B2 phase 4; docs/B2_DESIGN.md, "Phase 4
 * design addendum").  The edits are planned on a check of the unit as it
 * is (cindex_rename_plan), then the unit is checked again with them
 * applied.  The rename stands only if the two runs give the same events,
 * decls and diagnostics, up to the moved offsets and the new name.  That
 * comparison is the conflict check: the checker's own name lookup, for
 * every scope and namespace, rather than an imitation of it. */
#include "c/csymidx.h"
#include "c/frontend.h"
#include "pp.h"

#include <string.h>

typedef struct Run {
    TU tu;
    CIndex *ix;
    VEC(SrcRange) skipped;   /* inactive #if groups */
    char *missing;           /* the first #include not found */
    const CRename *q;
    bool live;
    const char *mpath;       /* the second run: the main file's path ... */
    const char *text;        /* ... and its edited text */
    size_t len;
} Run;

static bool run_overlay(void *ctx, const char *path, const char **buf,
                        size_t *len)
{
    Run *r = ctx;
    if (r->text && !strcmp(path, r->mpath)) {
        *buf = r->text;
        *len = r->len;
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

/* Checks the unit into r, with its symbol index; false if it could not. */
static bool run_check(Run *r, const CRename *q)
{
    PPListener l;
    FrontendOpts fo;
    r->q = q;
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
    return frontend_run(&r->tu, q->main, &fo) && r->ix;
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

/* Where offset off of the main file is after the edits; *edited: an edit
 * starts there. */
static uint32_t map_off(const CRename *q, uint32_t nl, uint32_t off, bool *edited)
{
    size_t lo = 0, hi = q->n;
    while (lo < hi) {        /* the edits before off */
        size_t mid = (lo + hi) / 2;
        if (q->offs[mid] < off)
            lo = mid + 1;
        else
            hi = mid;
    }
    *edited = lo < q->n && q->offs[lo] == off;
    return off + (uint32_t)lo * nl - (uint32_t)lo * q->len;
}

/* The original offset of offset y of the edited text (an edit's start for a
 * place inside the new name). */
static uint32_t unmap_off(const CRename *q, uint32_t nl, uint32_t y)
{
    uint32_t k;
    for (k = 0; k < q->n; k++) {
        uint32_t p = q->offs[k] + k * nl - k * q->len;
        if (y < p)
            break;
        if (y < p + nl)
            return q->offs[k];
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
                         StrBuf *why)
{
    const CIdxDecl *x = &a->ix->decls[d];
    const CIdxScope *s = x->scope ? &a->ix->scopes[x->scope - 1] : NULL;
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
            sb_printf(why, "'%s' occurs in an inactive #if group at %s, which "
                      "cannot be checked", w,
                      cindex_place(&a->tu.sm, f->path, k, at, sizeof at));
            return true;
        }
    }
    return false;
}

/* ---- the comparison (A1) ------------------------------------------------------ */

typedef struct Cmp {
    Run *a, *b;              /* the original run, the edited one */
    const CRename *q;
    uint32_t nl;             /* the new name's length */
    int mf;                  /* the main file's entry (the same in both) */
    char p1[512], p2[512];
} Cmp;

/* The place in the original text of an event of a's index or (second) of
 * b's. */
static const char *ev_place(Cmp *c, const CIdxEvent *e, bool second, char *buf)
{
    uint32_t off = second && (int)e->file == c->mf ? unmap_off(c->q, c->nl, e->off)
                                                   : e->off;
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
    off = la - fa->base;
    if (!strcmp(fa->path, c->b->mpath))
        off = map_off(c->q, c->nl, off, &ed);
    return off == lb - fb->base;
}

static const char *diag_place(Cmp *c, Run *r, SrcLoc loc)
{
    SrcFile *f = loc ? srcmgr_file_of(&r->tu.sm, loc) : NULL;
    uint32_t off;
    if (!f)
        return "?";
    off = loc - f->base;
    if (r == c->b && !strcmp(f->path, c->b->mpath))
        off = unmap_off(c->q, c->nl, off);
    return cindex_place(&c->a->tu.sm, f->path, off, c->p1, sizeof c->p1);
}

/* Appends to why how the edited run differs from the original; false if it
 * does not.  d: the renamed decl.  Diagnostics first: they say best what
 * went wrong (a redeclaration, a duplicate member); a capture has none. */
static bool differs(Cmp *c, uint32_t d, StrBuf *why)
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
        const char *want = i == d ? c->q->name : cindex_name(x, i);
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
            off = (int)e->file == c->mf ? map_off(c->q, c->nl, e->off, &ed) : e->off;
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

char *c_rename(CRename *q)
{
    Run a, b;
    Cmp *c = NULL;
    StrBuf why = {0}, text = {0};
    char msg[1024];
    const char *err, *old;
    uint32_t *ev = NULL, d, last = 0;
    size_t n = 0, k;
    SrcFile *mf = NULL;
    Ident *id;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    q->offs = NULL;
    q->n = 0;
    q->len = 0;
    if (run_check(&a, q))
        mf = cindex_srcfile(&a.tu.sm, path_normalize(&a.tu.arena, q->main));
    if (!mf) {
        sb_puts(&why, "the unit could not be checked");
        goto out;
    }
    if ((err = cindex_rename_plan(a.ix, &a.tu.sm, q->path, q->off, mf->path,
                                  &ev, &n, msg, sizeof msg)) != NULL) {
        sb_puts(&why, err);
        goto out;
    }
    d = a.ix->ev[ev[0]].decl;
    old = cindex_name(a.ix, d);
    q->len = (uint32_t)strlen(old);
    id = intern_find(a.tu.in, q->name, strlen(q->name));
    if (!cindex_is_identifier(q->name))
        sb_printf(&why, "'%s' is not an identifier", q->name);
    else if (id && id->ckw)
        sb_printf(&why, "'%s' is a keyword", q->name);
    else if (id && mt_hist(a.tu.pp.mt, id))
        sb_printf(&why, "'%s' is a macro name in this unit", q->name);
    else if (a.missing)
        sb_printf(&why, "#include %s was not found: uses of '%s' there cannot "
                  "be seen", a.missing, old);
    else
        inactive_use(&a, d, old, q->name, &why);
    if (why.len)
        goto out;
    /* the edited main file, then the comparison */
    q->offs = xmalloc((n + 1) * sizeof *q->offs);
    q->n = n;
    for (k = 0; k < n; k++) {
        q->offs[k] = a.ix->ev[ev[k]].off;
        sb_putn(&text, mf->buf + last, q->offs[k] - last);
        sb_puts(&text, q->name);
        last = q->offs[k] + q->len;
    }
    sb_putn(&text, mf->buf + last, mf->size - last);
    b.mpath = mf->path;
    b.text = sb_cstr(&text);
    b.len = text.len;
    if (!run_check(&b, q)) {
        sb_puts(&why, "the renamed unit could not be checked");
        goto out;
    }
    c = xcalloc(1, sizeof *c);
    c->a = &a;
    c->b = &b;
    c->q = q;
    c->nl = (uint32_t)strlen(q->name);
    c->mf = cindex_file(a.ix, mf->path);
    differs(c, d, &why);
out:
    free(c);
    run_free(&b);
    run_free(&a);
    free(ev);
    sb_free(&text);
    if (!why.len)
        return NULL;
    free(q->offs);
    q->offs = NULL;
    q->n = 0;
    sb_cstr(&why);
    return why.data;
}
