/* analysis.c - wiring for the preprocessor analyses. */
#include "analysis.h"

#include <string.h>

bool an_user_file(const SrcFile *f)
{
    return f && f->kind == SF_USER && !f->system_header;
}

bool an_user_loc(Analysis *a, SrcLoc loc)
{
    return an_user_file(srcmgr_file_of(a->sm, loc));
}

Skeleton *an_skeleton(Analysis *a, SrcFile *f)
{
    return skel_get(a->arena, a->sm, a->in, a->pp->opt->lex, f);
}

bool is_c_keyword(const char *s, size_t n)
{
    static const char *const kw[] = {
        "auto", "break", "case", "char", "const", "continue", "default", "do",
        "double", "else", "enum", "extern", "float", "for", "goto", "if",
        "inline", "int", "long", "register", "restrict", "return", "short",
        "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
        "unsigned", "void", "volatile", "while", "_Bool", "_Complex",
        "_Imaginary", NULL};
    int i;
    for (i = 0; kw[i]; i++)
        if (strlen(kw[i]) == n && memcmp(kw[i], s, n) == 0)
            return true;
    return false;
}

void analysis_attach(Analysis *a, PP *pp)
{
    a->pp = pp;
    a->arena = pp->arena;
    a->diag = pp->diag;
    a->sm = pp->sm;
    a->in = pp->in;
    hygiene_attach(a);
    cond_attach(a);
    include_attach(a);
}

void analysis_finish(Analysis *a)
{
    hygiene_finish(a);
    cond_finish(a);
    include_finish(a);
}

/* ---- parallel runs -------------------------------------------------- */

static void *an_fork(void *ctx, PP *wpp)
{
    Analysis *a = ctx, *w = NEW(wpp->arena, Analysis);
    *w = *a;
    w->pp = wpp;
    w->arena = wpp->arena;
    w->diag = wpp->diag;
    hygiene_fork(a, w);
    include_fork(a, w);
    /* cond: #if/#ifdef events only, all in phase A */
    return w;
}

static void an_join(void *ctx, void *wctx, uint32_t from, uint32_t to)
{
    include_join(ctx, wctx, from, to);
}

static void an_release(void *ctx, void *wctx)
{
    (void)ctx;
    hygiene_release(wctx);
    include_release(wctx);
}

ParClient analysis_par_client(Analysis *a)
{
    ParClient c;
    memset(&c, 0, sizeof c);
    c.ctx = a;
    c.fork = an_fork;
    c.join = an_join;
    c.release = an_release;
    return c;
}
