/* variadic functions, va_list passed on and copied, aggregates and doubles as extra arguments; compound literals */
#include <stdarg.h>

struct P { int x, y; };
struct Big { long a, b, c; };
struct Odd { int a, b, c; };

static int sum(int n, ...)
{
    va_list ap;
    int s = 0, i;
    va_start(ap, n);
    for (i = 0; i < n; i++)
        s += va_arg(ap, int);
    va_end(ap);
    return s;
}

static double dsum(int n, ...)
{
    va_list ap;
    double s = 0;
    va_start(ap, n);
    while (n-- > 0)
        s += va_arg(ap, double);
    va_end(ap);
    return s;
}

static long mixed(const char *fmt, ...)
{
    va_list ap;
    long r = 0;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt == 'i')
            r += va_arg(ap, int);
        else if (*fmt == 'l')
            r += va_arg(ap, long) * 10;
        else if (*fmt == 'd')
            r += (long)va_arg(ap, double) * 100;
        else if (*fmt == 'p') {
            struct P p = va_arg(ap, struct P);
            r += (p.x + p.y) * 1000;
        } else if (*fmt == 'B') {
            struct Big b = va_arg(ap, struct Big);
            r += (b.a + b.b + b.c) * 10000;
        } else if (*fmt == 'O') {
            struct Odd o = va_arg(ap, struct Odd);
            r += (o.a + o.b * 2 + o.c * 3) * 100000;
        } else if (*fmt == 's')
            r += *va_arg(ap, const char *);
    }
    va_end(ap);
    return r;
}

static int vsum(int n, va_list ap)
{
    int s = 0;
    while (n-- > 0)
        s += va_arg(ap, int);
    return s;
}

static int twice(int n, ...)
{
    va_list ap, cp;
    int a, b;
    va_start(ap, n);
    va_copy(cp, ap);
    a = vsum(n, ap);
    b = vsum(n, cp);
    va_end(cp);
    va_end(ap);
    return a * 1000 + b;
}

static int none(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    va_end(ap);
    return n;
}

static int dot(const int *a, const int *b) { return a[0] * b[0] + a[1] * b[1]; }

static int psum(const struct P *p, int n)
{
    int s = 0;
    while (n-- > 0)
        s += p[n].x * p[n].y;
    return s;
}

int main(void)
{
    struct P p = {3, 4};
    struct Big b = {1, 2, 3};
    int r = 0, i;
    r += sum(0) + sum(3, 1, 2, 3) + sum(5, 10, 20, 30, 40, 50);
    r += (int)dsum(3, 1.5, 2.25, 3.0) + (int)(dsum(1, 0.5f) * 10);
    r += (int)mixed("ilddpBOis", 1, 2L, 3.9, 4.1, p, b, (struct Odd){1, 2, 3}, 5, "A");
    r += twice(4, 5, 6, 7, 8) + none(9, 1, 2, 3);
    r += psum((struct P[]){{1, 2}, {3, 4}, {5, 6}}, 3);
    r += (int){7} + (int[]){1, 2, 3}[2] + ((struct P){.y = 5}).y + ((struct P){0}).x;
    r += dot((int[]){1, 2}, (int[]){3, 4}) + ((struct P){}).x;
    for (i = 0; i < 3; i++) {
        int *q = (int[]){i, i * 2};
        r += q[0] + q[1];
    }
    return r;
}
