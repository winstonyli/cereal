/* fixptok.c - see fixptok.h. */
#include "fixptok.h"

static bool ptok_plain(const PTok *t)
{
    return t->mloc == 0 && (!t->exp || t->exp == t->t.loc);
}

bool fix_replace_toks(DiagEngine *d, Diagnostic *dg, int owner,
                      const SrcMgr *sm, const Interner *in, const PTok *first,
                      const PTok *last, const char *text, FixApplic app)
{
    const PTok *t;
    for (t = first; t <= last; t++)
        if (!ptok_plain(t))
            return diag_fixit(d, dg, owner, 0, 0, text, app);
    return diag_fixit(d, dg, owner, first->t.loc,
                      last->t.loc + tok_raw_len(sm, in, &last->t), text, app);
}

bool fix_insert_before(DiagEngine *d, Diagnostic *dg, int owner,
                       const PTok *t, const char *text, FixApplic app)
{
    if (!ptok_plain(t))
        return diag_fixit(d, dg, owner, 0, 0, text, app);
    return diag_fixit(d, dg, owner, t->t.loc, t->t.loc, text, app);
}

bool fix_insert_after(DiagEngine *d, Diagnostic *dg, int owner,
                      const SrcMgr *sm, const Interner *in, const PTok *t,
                      const char *text, FixApplic app)
{
    SrcLoc e;
    if (!ptok_plain(t))
        return diag_fixit(d, dg, owner, 0, 0, text, app);
    e = t->t.loc + tok_raw_len(sm, in, &t->t);
    return diag_fixit(d, dg, owner, e, e, text, app);
}
