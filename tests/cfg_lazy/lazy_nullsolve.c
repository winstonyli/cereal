/* Whether a pointer cell is NULL is the solver's (docs/LAZY_BLOBS_DESIGN.md D11a): the guard on the scalar is a
 * one-in-4-billion draw, so the null test behind it only ever shows up in a solved call, and the solver has to ask for
 * NULL (ns_null) or for not NULL (ns_set: the first solved call that passes the guard sees a NULL cell, because the
 * sweep draws at most one NULL per call and it is this one). */
struct ns_rec { struct ns_rec *next; unsigned tag; };

int ns_null(const struct ns_rec *p)
{
    if (p->tag != 0x5a5a1234u)
        return 0;
    return p->next ? 2 : 1;
}

int ns_deep(const struct ns_rec *p)
{
    if (p->tag != 0x5a5a1234u)
        return 0;
    if (!p->next)
        return 1;
    if (p->next->tag != 0x1234abcdu)
        return 2;
    return p->next->next == 0 ? 4 : 3;
}
