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

typedef struct { int a, b; } P2;
typedef struct { double x, y; } D2;
typedef struct { long l; double d; } M2;
typedef struct { double d; int i; } DI;

long long mine_ag(int k, ...);

long long ext_ag(int k, ...)
{
    va_list ap;
    long long s = 0;
    va_start(ap, k);
    while (k-- > 0) {
        P2 p = va_arg(ap, P2);
        D2 d = va_arg(ap, D2);
        M2 m = va_arg(ap, M2);
        DI e = va_arg(ap, DI);
        s = s * 3 + p.a - p.b * 2 + (long long)(d.x * 4 - d.y) + m.l * 5 + (long long)m.d + (long long)e.d * 7 + e.i + va_arg(ap, long);
    }
    va_end(ap);
    return s;
}

long long gcc_callag(int k)
{
    P2 p = {k, 3};
    D2 d = {1.5, (double)k};
    M2 m = {k * 2L, 2.5};
    DI e = {4.0, k};
    return mine_ag(2, p, d, m, e, 7L, p, d, m, e, 8L);
}
