/* fixtu.c - one translation unit's side of `cereal fix` (docs/C_DESIGN.md
 * 6.2, "Collect"): the check and the lint analyses in one run, with what
 * the merge and the gate need kept in a FixResult. */
#include "fixres.h"

#include "analysis/analysis.h"
#include "c/frontend.h"
#include "pp.h"

#include <limits.h>
#include <string.h>

static bool ov_serve(void *ctx, const char *path, const char **buf, size_t *len)
{
    const FixResult *r = ctx;
    size_t i;
    for (i = 0; i < r->nov; i++)
        if (!strcmp(r->ov[i].path, path)) {
            *buf = r->ov[i].buf;
            *len = r->ov[i].len;
            return true;
        }
    return false;
}

typedef VEC(SrcRange) RangeVec;

static void on_skipped(void *ctx, SrcLoc b, SrcLoc e)
{
    SrcRange x;
    x.begin = b;
    x.end = e;
    vec_push((RangeVec *)ctx, x);
}

static uint64_t fnv(const char *s, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    while (n--)
        h = (h ^ (unsigned char)*s++) * 0x100000001B3ull;
    return h;
}

void fixres_free(FixResult *r)
{
    vec_free(&r->files);
    vec_free(&r->diags);
    vec_free(&r->edits);
    vec_free(&r->groups);
    vec_free(&r->inactive);
    free(r->enabled);
    arena_free(&r->arena);
    memset(r, 0, sizeof *r);
}

/* Index in r->files of the user file holding loc, or -1. */
static int32_t user_index(const int32_t *by_id, const SrcMgr *sm, SrcLoc loc,
                          SrcFile **fp)
{
    SrcFile *f = loc ? srcmgr_file_of(sm, loc) : NULL;
    *fp = f;
    if (!f || f->kind != SF_USER || f->id < 0)
        return -1;
    return by_id[f->id];
}

static bool only_parens(const char *s)
{
    if (!*s)
        return false;
    for (; *s; s++)
        if (*s != '(' && *s != ')')
            return false;
    return true;
}

static void collect(FixResult *r, TU *tu, const RangeVec *skipped)
{
    SrcMgr *sm = &tu->sm;
    uint32_t nf = srcmgr_nfiles(sm), k;
    int32_t *by_id = xmalloc((nf ? nf : 1) * sizeof *by_id);
    size_t i;
    for (k = 0; k < nf; k++)
        by_id[k] = -1;
    for (k = 0; k < nf; k++) {
        SrcFile *f = srcmgr_file(sm, k);
        if (f->kind == SF_USER && f->id >= 0 && (uint32_t)f->id < nf) {
            FFile ff;
            char rp[PATH_MAX];
            ff.path = arena_strdup(&r->arena, f->path);
            ff.key = arena_strdup(&r->arena, realpath(f->path, rp) ? rp : f->path);
            ff.hash = fnv(f->buf, f->size);
            ff.size = f->size;
            by_id[f->id] = (int32_t)r->files.len;
            vec_push(&r->files, ff);
        }
    }
    for (i = 0; i < tu->diag.all.len; i++) {
        const Diagnostic *d = tu->diag.all.data[i];
        FDiag fd;
        FGroup g;
        SrcFile *f;
        bool nofix;
        size_t j;
        if (!d || d->level == DL_IGNORED)
            continue;
        memset(&fd, 0, sizeof fd);
        fd.level = (uint8_t)d->level;
        fd.id = d->id ? arena_strdup(&r->arena, d->id) : "";
        fd.msg = arena_strdup(&r->arena, d->msg ? d->msg : "");
        fd.file = user_index(by_id, sm, d->loc, &f);
        if (fd.file >= 0) {
            fd.off = srcmgr_offset(f, d->loc);
            srcmgr_linecol(f, d->loc, &fd.line, &fd.col);
        }
        for (j = 0; j < d->notes.len && !fd.hdr; j++)
            fd.hdr = d->notes.data[j].hdr ? arena_strdup(&r->arena, d->notes.data[j].hdr) : NULL;
        if (d->level >= DL_ERROR)
            r->nerrors++;
        vec_push(&r->diags, fd);
        if (!d->fixits.len)
            continue;
        nofix = d->nofix;
        for (j = 0; j < d->notes.len; j++)
            nofix |= d->notes.data[j].nofix;
        if (nofix)
            continue;
        memset(&g, 0, sizeof g);
        g.id = fd.id;
        g.level = fd.level;
        g.app = FIX_SAFE;
        g.paren = true;
        g.diag = (uint32_t)(r->diags.len - 1);
        g.e0 = (uint32_t)r->edits.len;
        g.pfile = -1;
        for (j = 0; j < d->fixits.len; j++) {
            const FixIt *x = &d->fixits.data[j];
            FEdit e;
            SrcFile *bf;
            int32_t fi = user_index(by_id, sm, x->begin, &bf);
            if (fi < 0)
                break;
            if (x->owner) {         /* a note's fix-it on the diagnostic's own
                                       text is another way to the same end
                                       (xor: "0x2" for "1 <<"): not both */
                size_t m;
                for (m = 0; m < d->fixits.len; m++) {
                    const FixIt *y = &d->fixits.data[m];
                    if (!y->owner && x->begin <= y->end && y->begin <= x->end)
                        break;
                }
                if (m < d->fixits.len)
                    continue;
            }
            e.file = (uint32_t)fi;
            e.begin = srcmgr_offset(bf, x->begin);
            e.end = srcmgr_offset(bf, x->end);
            e.text = arena_strdup(&r->arena, x->text);
            if (x->owner && !g.hdr && d->notes.data[x->owner - 1].hdr)
                g.hdr = arena_strdup(&r->arena, d->notes.data[x->owner - 1].hdr);
            if (x->app > g.app)
                g.app = x->app;
            if (e.begin != e.end || !only_parens(e.text))
                g.paren = false;
            if (e.end - e.begin != strlen(e.text) ||
                memcmp(bf->buf + e.begin, e.text, e.end - e.begin))
                g.change = true;    /* gcc offers "%u" for a "%u" */
            if (!g.ne) {
                g.pfile = fi;
                srcmgr_linecol(bf, x->begin, &g.pline, &g.pcol);
            }
            vec_push(&r->edits, e);
            g.ne++;
        }
        if (j < d->fixits.len || !g.change) {   /* outside the user files, or
                                                   a no-op */
            r->edits.len = g.e0;
            continue;
        }
        vec_push(&r->groups, g);
    }
    for (i = 0; i < skipped->len; i++) {
        SrcFile *f;
        int32_t fi = user_index(by_id, sm, skipped->data[i].begin, &f);
        FRange x;
        if (fi < 0)
            continue;
        x.file = (uint32_t)fi;
        x.begin = srcmgr_offset(f, skipped->data[i].begin);
        x.end = skipped->data[i].end > skipped->data[i].begin
                    ? srcmgr_offset(f, MIN(skipped->data[i].end,
                                           f->base + f->size))
                    : x.begin;
        vec_push(&r->inactive, x);
    }
    r->enabled = diag_enabled_map(&tu->diag);
    free(by_id);
}

int fix_tu(Options *o, const char *path, FILE *out TU_UNUSED, FILE *err,
           void *res)
{
    FixResult *r = res;
    TU tu;
    Analysis an;
    PPListener l;
    FrontendOpts fo;
    RangeVec skipped = {0};
    tu_init(&tu, o);
    tu.diag.out = err;
    tu.diag.fatal_errors = false;           /* -Wfatal-errors is off for fix */
    if (r->nov) {
        tu.sm.overlay = ov_serve;
        tu.sm.overlay_ctx = r;
    }
    memset(&an, 0, sizeof an);
    analysis_attach(&an, &tu.pp);
    memset(&l, 0, sizeof l);
    l.ctx = &skipped;
    l.skipped = on_skipped;
    pp_add_listener(&tu.pp, l);
    memset(&fo, 0, sizeof fo);
    fo.check = true;
    fo.digest = &r->digest;
    r->opened = frontend_run(&tu, path, &fo);
    if (r->opened) {
        analysis_finish(&an);
        collect(r, &tu, &skipped);
    } else {
        analysis_discard(&an);
        if (!r->quiet)
            fprintf(err, "cereal: error: %s: cannot open the file\n", path);
    }
    vec_free(&skipped);
    tu_free(&tu);
    return r->opened ? 0 : 3;
}
