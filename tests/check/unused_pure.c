// flags: -std=c99 -Wunused-value
#include <string.h>
int pf(int) __attribute__((pure));
int cf(int) __attribute__((const));
int sf(int);
void h(char *d, int b)
{
  strlen(d);
  __builtin_memchr(d, 0, 4);
  memcmp(d, d, 3);
  pf(1);
  cf(b);
  sf(1);
  strlen(d), sf(2);
  sf(1), 1, sf(2);
  sf(1) + sf(2), sf(3);
  for (b; b < 3; b)
    sf(0);
  for (b = 0, b; b < 3; b++, b)
    sf(0);
}
