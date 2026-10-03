// flags: -Wformat -Wformat-signedness
#include <stddef.h>
char c; unsigned char uc; signed char sc; short s; unsigned short us; int i; unsigned u;
long l; unsigned long ul; long long ll; unsigned long long ull; size_t z; _Bool b; enum { A } e;
void f(void)
{
  __builtin_printf("%d %d %d %d %d %d %d\n", c, uc, sc, us, u, ul, b);
  __builtin_printf("%u %u %u %u %u %u %u\n", c, i, l, ll, z, e, b);
  __builtin_printf("%x %o %X\n", i, l, ll);
  __builtin_printf("%ld %lu %lld %llu\n", ul, l, ull, ll);
  __builtin_printf("%zu %zd\n", z, z);
  __builtin_printf("%hhu %hd %c\n", i, u, u);
}
