// flags: -std=c99 -pedantic
#include <stddef.h>
int printf(const char *, ...);
void a(char *s)
{
  printf("%zu %zd %zx %zo", s, s, s, s);
  printf("%tu %td %tx", s, s, s);
  printf("%ju %jd %jx", s, s, s);
  printf("%lc %C", s, s);
  printf("%zn %tn %jn %lln %hhn", s, s, s, s, s);
  printf("%qd %qu", s, s);
  printf("%lu %llu %llx %lo %lld %lli", s,s,s,s,s,s);
  printf("%Lu %Lx", s, s);
  printf("%hu %hhx %ho", s, s, s);
  printf("%Zu", s);
  printf("%a %Lg %LE", s, s, s);
  printf("%p %n %s %c %%", 1, 2, 3, s);
}
void b(char *s, unsigned char *us, const void *cv, volatile void *vv, void **pv, const int *ci, volatile int *vi, const char *cs, unsigned *up, long long *llp, int **ipp, void (*fn)(void), char (*pa)[3])
{
  printf("%p", s);
  printf("%p", us);
  printf("%p", cv);
  printf("%p", vv);
  printf("%p", pv);
  printf("%p", fn);
  printf("%p", pa);
  printf("%n", ci);
  printf("%n", vi);
  printf("%n", up);
  printf("%lln", llp);
  printf("%p", ipp);
  printf("%s", cv);
  printf("%s", pa);
  printf("%d", pa);
}
void c(int i, double d, char *s)
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
