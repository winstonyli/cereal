/* A walk over a linked list of lazy blobs, each element's pointer subtracted from one field of the head (the open-upvalue list of Lua's
 * luaD_reallocstack): the retry must make every v share the object of stack by a hint for the cell's class, not one retry per element
 * (four retries stop at the fifth).  LS.open and LU.next sit at the same offset (8), so the first v and the later ones fall in one
 * class by this layout; if the structs change they split and need one more retry. */
struct LU { char *v; struct LU *next; };
struct LS { char *stack; struct LU *open; };

long ll_run(struct LS *s)
{
    char *base = s->stack;
    struct LU *u = s->open;
    long sum = 0;
    int i;
    for (i = 0; i < 6; i++, u = u->next)
        sum += u->v - base;
    return sum;
}
