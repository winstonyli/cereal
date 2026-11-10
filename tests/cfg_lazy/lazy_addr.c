/* Functions that turn a drawn pointer into an integer (docs/LAZY_ADDR_DESIGN.md): a lazy blob has a fixed address on both sides, so the
 * integer is the same.  la_ctl is the control (no integer); la_back and la_pun turn the integer back into a pointer and dereference it,
 * which the evaluator stops on; la_own converts a pointer into the function's own block, which stays unsupported. */
void *malloc(unsigned long);

struct LA { char *a; char *b; unsigned long v; };

unsigned long la_align(struct LA *s)
{
    return (unsigned long)s->a & 63;
}

unsigned long la_diff(struct LA *s)
{
    if (!s->a || !s->b)
        return 0;
    return (unsigned long)s->b - (unsigned long)s->a;
}

unsigned long la_hash(struct LA *s)
{
    s->v = (unsigned long)s->a >> 4;
    return s->v + 1;
}

int la_ctl(struct LA *s)
{
    if (!s->a)
        return 0;
    return *s->a;
}

int la_back(struct LA *s)
{
    if (!s->a)
        return 0;
    return *(char *)((unsigned long)s->a & ~63ul);
}

int la_pun(struct LA *s)
{
    if (!s->a)
        return 0;
    s->v = (unsigned long)s->a;
    return **(char **)&s->v;
}

unsigned long la_own(struct LA *s)
{
    char *p = malloc(8);
    return (unsigned long)p;
}
