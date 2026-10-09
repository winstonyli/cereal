/* A file including itself (gcc.dg/range-test-1.c): the block of the second
 * inclusion has the same path as the function body around the #include but
 * lies after it, so it is dropped and k takes the body scope */
#ifndef AGAIN
#define AGAIN
int main(void)
{
    int n = 0;
#include "selfinc.c"
    return n;
}
#else
    { int k = n; n += k; }
#endif
