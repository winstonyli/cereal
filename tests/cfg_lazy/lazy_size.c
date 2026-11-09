/* Ten pointer fields of one pointee size at different offsets of one struct, each subtracted from an eleventh (the stack, top, tbclist and
 * upvalue cells of Lua's luaD_reallocstack are all StackValue *): the retry must make them all share the object of base by one hint for
 * the pointee size.  A hint per cell, or per offset class, needs one retry per field and stops at the ninth (LMAXHINT 8). */
struct SZ { char *base; char *p0, *p1, *p2, *p3, *p4, *p5, *p6, *p7, *p8, *p9; };

long sz_run(struct SZ *s)
{
    char *b = s->base;
    long sum = 0;
    sum += s->p0 - b;
    sum += s->p1 - b;
    sum += s->p2 - b;
    sum += s->p3 - b;
    sum += s->p4 - b;
    sum += s->p5 - b;
    sum += s->p6 - b;
    sum += s->p7 - b;
    sum += s->p8 - b;
    sum += s->p9 - b;
    return sum;
}
