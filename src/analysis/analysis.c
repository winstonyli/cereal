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
