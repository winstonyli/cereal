/* fixtok.c - see fixtok.h. */
#include "fixtok.h"

bool fix_tok_replace(DiagEngine *d, Diagnostic *dg, int owner,
                     const SrcMgr *sm, const Interner *in, const Tok *t,
                     const char *text, FixApplic app)
{
    if (t->flags & (TF_ORIGIN_BODY | TF_ORIGIN_ARG | TF_PASTED | TF_SYNTH))
        return diag_fixit(d, dg, owner, 0, 0, text, app);
    return diag_fixit(d, dg, owner, t->loc, t->loc + tok_raw_len(sm, in, t),
                      text, app);
}
