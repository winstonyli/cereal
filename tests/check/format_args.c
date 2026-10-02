// flags: -std=c99 -pedantic
#include <stddef.h>
typedef unsigned long UL;
int printf(const char *, ...);
int sprintf(char *, const char *, ...);
void a(int i, long l, unsigned u, double d, char *s, void *p, size_t z, float *fp, short sh, char c, long long ll, int *ip, UL ul)
{
  printf("%s", i);
  printf("a\tb %d %s", l, i);
  printf("%d", z);
  printf("%d", ul);
  printf("%p", fp);
  printf("%d");
  printf("%s %d", s);
  printf("%d %d", i, i, i);
  printf("%d\n", i, i);
  printf("%", i);
  printf("%y", i);
  printf("%ld", i);
  printf("%lu %u %x", i, i, l);
  printf("%f", i);
  printf("%d", d);
  printf("%c", s);
  printf("%d", s);
  printf("%d", p);
  printf("%s", ip);
  printf("%n", i);
  printf("%hd %hhd", sh, c);
  printf("%lld", l);
  printf("%.*d", l, i);
  printf("%*d", i, i);
  printf("%5.2s", i);
  printf("%%");
  printf("%d" "%s", i, i);
  sprintf(s, "%d", d);
  printf("%1", i);
  printf("");
  printf("%z", i);
  printf("%lf", d);
  printf("%Lf", d);
  printf("%s", "x", 1);
  printf("%d", 1.0);
  printf("%d", 1UL);
  printf("%d", (char)1);
  printf("%i %o %X %e %g %a %A %E %G", d, d, d, i, i, i, i, i, i);
}
void b(int i, double d, char *s)
{
  printf("%+ d", i);
  printf("%-05d", i);
  printf("%05.3d", i);
  printf("%--d", i);
  printf("%05.3f", d);
  printf("%0s", s);
  printf("%5n", &i);
  printf("%#s", s);
  printf("%'d %Id", i, i);
  printf("%#x %#o %#u", i, i, i);
  printf("%+c", i);
  printf("%.3p", s);
  printf("%-+ 05d", i);
  printf("%5%");
  printf("%l%");
  printf("%c%", i);
  printf("%.*d", i);
  printf("%*d");
  printf("%1$d", i);
  printf("%hhhd", i);
  printf("%llld", i);
  printf("%lhd", i);
  printf("%zzd", i);
  printf("%5.5.5d", i);
  printf("%d%s", i, s, s);
  printf("%d");
}
