/* A field read as a pointer and then set as an integer (a union slot, as Lua's TValue is): the integer covers the whole
 * pointer slot, so gcc and the evaluator agree on the bytes afterwards; the sweep must run the call, not stop at the store. */
typedef union Slot { long *p; unsigned long u; } Slot;
typedef struct Box { Slot s; long n; } Box;
long lo_run(Box *b)
{
    long r = b->s.p != 0;
    b->s.u = 7 + (unsigned long)b->n;
    return r + (long)b->s.u;
}
