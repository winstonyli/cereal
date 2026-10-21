// trap: out of bounds
#include <stdarg.h>

static int f(int n, ...)
{
    va_list ap;
    int s = 0;
    va_start(ap, n);
    while (n-- > 0)
        s += va_arg(ap, int);
    va_end(ap);
    return s;
}

int main(void)
{
    return f(3, 1, 2);
}
