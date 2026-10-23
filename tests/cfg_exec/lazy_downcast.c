/* The shape of Lua's luaV_execute under --eval-lazy (docs/LAZY_BLOBS_DESIGN.md): a pointer to a
 * header is cast to the larger struct it starts, and a pointer inside that decides the result. */
struct Hdr {
    struct Hdr *next;
    int tag;
};

struct Closure {
    struct Hdr h;
    int *k;
    int n;
};

struct Slot {
    struct Hdr *obj;
    int unused;
};

struct Frame {
    struct Slot *func;
};

int lz_run(struct Frame *ci)
{
    struct Hdr *o;
    struct Closure *cl;
    if (!ci->func)
        return -1;
    o = ci->func->obj;
    if (!o)
        return -2;
    cl = (struct Closure *)o;
    if (!cl->k)
        return -3;
    return cl->k[0] > 5 ? 1 : cl->n > 2 ? 2 : 3;
}
