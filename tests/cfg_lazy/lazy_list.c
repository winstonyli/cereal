/* A walk over a linked list of lazy blobs, each element's pointer subtracted from one field of the head (the open-upvalue list of Lua's
 * luaD_reallocstack): the retry must make every v share the object of stack by one hint for the pointee size (char, 1 byte, for stack
 * and every v), not one retry per element (four retries stop at the fifth).  The stack cell is read before its object exists, so it
 * must not take the share (the hint names an unmade blob). */
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
