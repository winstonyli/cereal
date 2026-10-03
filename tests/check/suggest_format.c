// flags: -Wmissing-format-attribute
#include <stdarg.h>
#include <stdio.h>
void a1 (const char *fmt, va_list ap) { vprintf (fmt, ap); }
void a2 (const char *fmt, va_list ap)
{
  vprintf (fmt, ap);
}
void a3 (const char *fmt, va_list ap)
{
  int x = 1;
  x++; vprintf (fmt, ap);
  vprintf (fmt, ap);
}
void a4 (const char *fmt, ...) { va_list ap; va_start (ap, fmt); vprintf (fmt, ap); va_end (ap); }
void a5 (const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  if (fmt)
    vprintf (fmt, ap);
  va_end (ap);
}
void a6 (int k, va_list ap, const char *fmt)
{ if (k) { vprintf (fmt,
  ap); } }
void my (const char *, va_list) __attribute__((format(printf, 1, 0)));
void mys (const char *, va_list) __attribute__((format(scanf, 1, 0)));
void mym (const char *, ...) __attribute__((format(printf, 1, 2)));
void d1 (const char *f, va_list ap) { my (f, ap); }
void d2 (const char *f, ...) __attribute__((format(printf, 1, 2)));
void d2 (const char *f, ...) { va_list ap; va_start (ap, f); my (f, ap); vprintf (f, ap); va_end (ap); }
void d3 (const char *f, va_list ap) __attribute__((format(scanf, 1, 0)));
void d3 (const char *f, va_list ap) { my (f, ap); mys (f, ap); }
void d4 (int x, va_list ap) { my ("lit %d", ap); vprintf ("lit", ap); }
void d5 (int x, va_list ap, char *const q) { my (q, ap); }
void d6 (int x, va_list ap, char **q) { my (*q, ap); }
void d7 (int x, va_list ap, unsigned char *q) { my ((char *) q, ap); }
void d8 (const char *f, ...) { mym (f, 1); }
void d9 (const char *f, va_list ap) { char b[10]; vsnprintf (b, 10, f, ap); vsprintf (b, f, ap); vfprintf (stderr, f, ap); }
void d10 (const char f[], va_list ap) { vprintf (f, ap); }
void foo0 (const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  __builtin_vprintf (fmt, ap);
  vprintf (fmt, ap);
  vfprintf (stdout, fmt, ap);
  va_end (ap);
}
void bar0 (const char *fmt, va_list ap) { vprintf (fmt, ap); }
void baz0 (const char *fmt, va_list ap) { __builtin_vprintf (fmt, ap); }
void qux0 (const char *fmt, ...) { va_list ap; va_start (ap, fmt); __builtin_vsnprintf (0, 0, fmt, ap); __builtin_vscanf(fmt, ap); va_end (ap); }
