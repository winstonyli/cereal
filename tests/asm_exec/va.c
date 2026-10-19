/* Variadic functions for tests/asm_exec.py: va_start/va_arg/va_copy in our
 * functions, called by us and (gcc/va.c) by gcc, and our calls into gcc's. */
#include <stdarg.h>

long long ext_vsum(int n, ...);     /* gcc-built */

long long vsum(int n, ...)          /* n long longs, then n doubles */
{
    va_list ap, ap2;
    long long s = 0;
    double d = 0;
    int i;
    va_start(ap, n);
    va_copy(ap2, ap);
    for (i = 0; i < n; i++)
        s += va_arg(ap, long long) * (i + 1);
    for (i = 0; i < n; i++)
        d += va_arg(ap, double);
    va_end(ap);
    for (i = 0; i < n; i++)
        s += va_arg(ap2, long long);
    va_end(ap2);
    return s + (long long)(d * 4);
}

int mixed(char *tag, int n, ...)    /* ints and pointers interleaved with doubles */
{
    va_list ap;
    int s = tag[0];
    va_start(ap, n);
    while (n-- > 0) {
        s += va_arg(ap, int);
        s += (int)va_arg(ap, double);
        s += *va_arg(ap, char *);
    }
    va_end(ap);
    return s;
}

static long long vhelper(int n, va_list ap)
{
    long long s = 0;
    while (n-- > 0)
        s = s * 3 + va_arg(ap, int);
    return s;
}

long long forward(int n, ...)
{
    va_list ap;
    long long r;
    va_start(ap, n);
    r = vhelper(n, ap);
    va_end(ap);
    return r;
}

long long mine_vsum(int n, ...)
{
    va_list ap;
    long long s = 0;
    va_start(ap, n);
    while (n-- > 0)
        s += va_arg(ap, long long) * 5;
    va_end(ap);
    return s;
}

int callv(int k)
{
    return (int)vsum(3, (long long)k, 2LL, k * 3LL, 1.5, 2.5, (double)k) +
           (int)vsum(9, 1LL, 2LL, 3LL, 4LL, 5LL, 6LL, 7LL, 8LL, (long long)k, 1.0, 2.0, 3.0, 4.0,
                     5.0, 6.0, 7.0, 8.0, 9.0);
}

int callmixed(int k)
{
    char a = 7, b = (char)k, c = 3;
    return mixed("x", 3, k, 1.5, &a, 2, 2.5, &b, 3, 3.5, &c) +
           mixed("y", 5, 1, 1.0, &a, 2, 2.0, &a, 3, 3.0, &a, 4, 4.0, &a, k, 5.0, &c);
}

int callfwd(int k)
{
    return (int)forward(4, k, 2, 3, 4) + (int)forward(8, 1, 2, 3, 4, 5, 6, 7, k);
}

int cross(int k)
{
    return (int)ext_vsum(4, (long long)k, 2LL, 3LL, 4LL) + (int)mine_vsum(10, 1LL, 2LL, 3LL, 4LL, 5LL, 6LL, 7LL, 8LL, 9LL, (long long)k);
}

typedef struct { int a, b; } P2;
typedef struct { double x, y; } D2;
typedef struct { long l; double d; } M2;
typedef struct { double d; int i; } DI;     /* sse, then integer eightbyte */

/* register-class aggregates through va_arg: k rounds of P2, D2, M2, DI, long */
long long mine_ag(int k, ...)
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

long long ext_ag(int k, ...);       /* gcc-built */

long long agcall(int k)
{
    P2 p = {k, 3};
    D2 d = {1.5, (double)k};
    M2 m = {k * 2L, 2.5};
    DI e = {4.0, k};
    return mine_ag(1, p, d, m, e, 9L) + mine_ag(3, p, d, m, e, 1L, p, d, m, e, 2L, p, d, m, e, 3L) +
           ext_ag(2, p, d, m, e, 5L, p, d, m, e, 6L);
}
