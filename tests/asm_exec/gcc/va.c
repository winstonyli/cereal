/* The gcc-built side of tests/asm_exec/va.c. */
#include <stdarg.h>

long long mine_vsum(int n, ...);
long long vsum(int n, ...);

long long ext_vsum(int n, ...)
{
    va_list ap;
    long long s = 0;
    va_start(ap, n);
    while (n-- > 0)
        s += va_arg(ap, long long) * 7;
    va_end(ap);
    return s;
}

int gcc_callmine(int k)
{
    return (int)mine_vsum(11, 1LL, 2LL, 3LL, 4LL, 5LL, 6LL, 7LL, 8LL, 9LL, 10LL, (long long)k) +
           (int)vsum(2, (long long)k, 5LL, 0.5, 1.25);
}
