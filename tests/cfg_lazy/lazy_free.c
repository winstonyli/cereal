/* Functions that free what the sweep drew (docs/LAZY_HEAP_DESIGN.md): a lazy blob is never freed, on either side, so the call runs on and
 * the final bytes agree with gcc.  lf_twice frees twice, frees into the middle and reads after the free; lf_own mixes a free of
 * NULL and of its own malloc with the frees of drawn blobs. */
void *malloc(unsigned long);
void free(void *);

struct LF { char *buf; int v; };

int lf_run(struct LF *s)
{
    int r = s->v;
    free(s->buf);
    free(s);
    return r;
}

int lf_twice(struct LF *s)
{
    free(s->buf);
    free(s->buf);
    if (s->buf)
        free(s->buf + 1);
    return s->v + (s->buf ? s->buf[0] : 0);
}

int lf_own(struct LF *s)
{
    int *p = malloc(8);
    int r;
    p[0] = s->v;
    r = p[0];
    free((void *)0);
    free(p);
    free(s->buf);
    return r;
}
