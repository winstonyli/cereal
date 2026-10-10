// flags: -Wunused-value
#include <stdlib.h>
/* A user function that shares its name with a __builtin_ suffix (expect,
 * prefetch, unreachable...) is not a pure built-in: its call has side
 * effects for gcc. */
static int expect(int *p, int x) { return *p == x; }
static int clz(unsigned x) { return (int)x; }
static int popcount(unsigned x) { return (int)x; }
void f(int *p, unsigned u)
{
    expect(p, 1);
    clz(u);
    popcount(u);
    abs(*p);                    /* library built-in, const: warns */
    __builtin_expect(*p, 1);    /* const: warns */
    __builtin_popcount(u);
}
