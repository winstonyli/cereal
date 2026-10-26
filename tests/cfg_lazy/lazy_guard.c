/* A guard behind a pointer chain under --eval-solve (docs/LAZY_BLOBS_SOLVER_PLAN.md): the
 * branch needs a byte the call draws two pointers in, which only the solver finds. */
struct Inner {
    int k[4];
};

struct Outer {
    struct Inner *in;
    int n;
};

int lg_run(struct Outer *o)
{
    if (!o->in)
        return -1;
    if (o->in->k[0] * 3 + 7 == 1000)
        return 9;
    return o->n > 2 ? 2 : 1;
}
