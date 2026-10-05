// flags: -std=gnu99 -pedantic-errors
/* A flexible array member after "no named members" stays a member (its
 * uses are silent).  A typeof whose type is not variably modified
 * drops the side effects of its operand.  A bare #if / #elif is reported at
 * the end of the line (after blanks and comments). */
struct t { char b[]; };
int g(struct t *q) { return q->b[0]; }

int sa[10];

int f(int m, int n)
{
    static int (*a5)[n] = (__typeof__((int (*)[m++])sa))sa;
    static int (*a6)[n] = (__typeof__((int (*)[100])(int (*)[m++])sa))sa;
    return n;
}

#if
#endif
#if  /* c */
#endif
#if 0
#elif
#endif
