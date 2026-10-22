#include "link.h"

int shared_base = 5;

static int hidden(int x)
{
    return x + 1;
}

int a_twice(int x)
{
    return 2 * x;
}

/* a call into b whose body has a guarded branch */
int a_call_guard(int x)
{
    return b_guard(x) + hidden(x);
}

/* a function pointer made here and called in b */
int a_use_fp(int x)
{
    return b_apply(a_twice, x);
}

/* a record by value into b, and one back */
int a_pair(int x, int y)
{
    struct pair p;
    p.a = x;
    p.b = y;
    return b_sum(p);
}

int a_make(int x)
{
    struct pair p = b_make(x);
    return p.a - p.b;
}

/* a global defined here, read here and in b */
int a_read_shared(int x)
{
    return shared_base + x;
}
