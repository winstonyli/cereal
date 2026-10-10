// flags: -Wall -Wformat-signedness -fdiagnostics-parseable-fixits
#include <stdio.h>
void f(long l, int i, double d, const char *s)
{
    printf("%d %ld %f %s\n", l, i, s, d);
    printf("%5.2ld|%-8d|%lu\n", i, l, i);
    scanf("%d %f", &l, &d);
}
void h(unsigned u, signed char c) { printf("%d %u\n", u, c); }
