#include <fakelib.h>
#include "ccalls.h"
#define CALL_LEAF(v) leaf(v, 0)
#define LEAF_MACRO 1
int leaf(int a, int b);
int g_impl(int x);
int extern_only(int x);
int helper(int v) { return v + g_impl(v); }
int init = sizeof(leaf(1, 2));
int a1 = sizeof(leaf(3, 4)), b1 = sizeof(extern_only(5));
_Static_assert(sizeof(leaf(1, 2)) == 4, "");
int leaf(int a, int b) { return a + b; }
int user(struct fake_ops *o, int n)
{
    n += leaf(1, 2) + helper(3) + in_header(4);
    n += CALL_LEAF(5);
    n += fake_len("abc") + undeclared_fn(9) + o->cb(1);
    n += extern_only(n);
    return n ? user(o, n - 1) : n;
}
