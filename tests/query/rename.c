#include "rename.h"
#define DECL(n) int n = 0
#define STMT(v) ({ int _t = (v); _t; })
#define PASTE(a) a##_n
#define BOTH(a) (a + a##_n)
#define GETG() gy
#define TWICE(e) ((e) + (e))
struct point { int x, y; };
typedef unsigned long size;
static int counter;
int total;
int gy;
int val, val_n;
int api(void) { return 1; }
static int helper(int a, int b)
{
    int sum = a + b;
    {
        int inner = sum;
        sum = inner + counter;
    }
#if 0
    sum = later;
#endif
    return sum;
}
static void fields(struct point *q)
{
    struct point p = {.x = 1, .y = 2};
    size n = sizeof p;
    q->x = p.x + (int)n;
}
static int labels(int k)
{
    if (k)
        goto out;
    k++;
out:
    return k;
again:
    goto again;
}
static int shadow(void)
{
    int v = 1;
    {
        int v = 2;
        total += v;
    }
    DECL(local);
    local += STMT(v);
    local += PASTE(val) + BOTH(val);
    {
        int k = 3;
        total += GETG() + k;
    }
    return v + local;
}
static int implicit(void)
{
    __builtin_va_list ap;
    (void)ap;
    return undeclared_fn();
}
#define TWO(n) ({ int n = 1; n; }) + n
static int both(void)
{
    int z = 0;
    z = TWO(z);
    return z;
}
