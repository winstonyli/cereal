/* Functions that realloc what the sweep drew (docs/LAZY_REALLOC_DESIGN.md): a lazy blob grows in place and the pointer stays, on either
 * side, so the call runs on and the final bytes agree with gcc.  lr_grow grows, writes the tail and shrinks; lr_twice reallocs twice and
 * into the middle; lr_own mixes realloc(NULL, n) and a realloc of its own block with the realloc of a drawn blob. */
void *malloc(unsigned long);
void *realloc(void *, unsigned long);

struct LR { char *buf; int v; };

int lr_grow(struct LR *s)
{
    int r = s->v;
    s->buf = realloc(s->buf, 256);
    if (s->buf) {
        s->buf[200] = 7;
        r += s->buf[200];
    }
    s->buf = realloc(s->buf, 8);
    if (s->buf)
        r += s->buf[0];
    return r;
}

int lr_twice(struct LR *s)
{
    s->buf = realloc(s->buf, 64);
    s->buf = realloc(s->buf, 128);
    if (s->buf)
        realloc(s->buf + 1, 160);
    return s->v + (s->buf ? s->buf[0] : 0);
}

int lr_own(struct LR *s)
{
    int *p = realloc(0, 8);
    p[0] = s->v;
    p = realloc(p, 64);
    s->buf = realloc(s->buf, 32);
    return p[0] + (s->buf ? 1 : 0);
}
