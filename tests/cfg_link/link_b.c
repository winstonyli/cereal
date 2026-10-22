#include "link.h"

static int hidden(int x)
{
    return x * 3;
}

int b_guard(int x)
{
    if (x == 12345)
        return hidden(x);
    return hidden(x) + 1;
}

int b_sum(struct pair p)
{
    return p.a * 10 + p.b;
}

int b_apply(int (*f)(int), int x)
{
    return f(x) + hidden(1);
}

struct pair b_make(int x)
{
    struct pair p;
    p.a = x;
    p.b = shared_base;
    return p;
}

int b_read_shared(int x)
{
    return shared_base * x;
}
