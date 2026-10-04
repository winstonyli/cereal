// flags: -std=c99 -Wdouble-promotion
extern int printf(const char *, ...);
extern void unproto();
float f, g; double d; long double ld; _Complex float cf; _Complex double cd; int i;
void t(void)
{
  f += d;
  f = f + 1.0;
  f = f + 1.0f;
  d = f * ld;
  d = d * f;
  cf = cf * cd;
  cf = cf * 2.0;
  i = f < 1.0;
  i = f < g;
  i = i ? f : ld;
  i = i ? f : g;
  i = i ?: f;
  printf("%f", f);
  printf("%f", d);
  printf("%f", (double)f);
  unproto(f, cf, d);
  unproto(
      f);
  i = sizeof(f + d) + sizeof(unproto(f));
  i = _Generic(f + d, double: 1, default: 2);
  f = -f + (f ? d : g);
}
