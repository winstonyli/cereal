/* Calls through function pointers a sweep draws (--eval-lazy: a pointer to a function type is NULL or one of
 * the functions of that type; docs/LAZY_BLOBS_DESIGN.md "Function pointers").  gcc must get the same function. */
typedef int (*fp_cb)(int *, int);

struct fp_hook {
    fp_cb cb;
    int *data;
};

int fp_add(int *p, int x)
{
    return *p + x + 1;
}

int fp_mul(int *p, int x)
{
    return *p * 2 + x;
}

static int fp_neg(int *p, int x)
{
    return -*p - x;
}

int fp_other(char *p)
{
    return *p;
}

int fp_run(struct fp_hook *h, int x)
{
    return h->cb ? h->cb(h->data ? h->data : &x, x) : -7;
}
