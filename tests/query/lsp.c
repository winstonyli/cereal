#include "lsp.h"
#define SQUARE(x) ((x) * (x))
#define CAT(a, b) a ## b
#define TWICE(x) SQUARE(x) + SQUARE(x)
#define FOO_BAR 7
int use(int v)
{
    int a = SQUARE(v);
    int b = TWICE(LIMIT);
    int c = CAT(FOO, _BAR);
#ifdef LIMIT
    a += LIMIT;
#endif
#undef SQUARE
#define SQUARE(x) (x)
    return a + b + c + SQUARE(2);
}
#if 0
int dead = SQUARE(3);
#endif
#if 0
#define GONE(x) (x)
int gone = GONE(1);
#endif
