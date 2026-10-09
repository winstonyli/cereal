// flags: -std=gnu11 -Wno-implicit-function-declaration
#include <stddef.h>
struct tm;
void f(double d, double *dp, double _Complex z, char *s, size_t n, struct tm *tm)
{
  lrint (d);
  llround (d);
  cabs (z);
  cacos (z);
  sincos (d, dp, dp);
  aligned_alloc (n, n);
  towlower (1);
  ilogbf (1);
  strftime (s, n, "", tm);
}
extern void g(char *, const char *, ...) __attribute__((format(strftime, 2, 0)));
void h(char *s) { g(s, ""); }
