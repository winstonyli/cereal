// flags: -Wunused-value -Wswitch-bool
#include <stdbool.h>
double _Complex x, y;
double d;
int i;
float _Complex fz;
int printf;
int abs;
extern int strcmp_not_builtin;
int f1(const char *, ...) __attribute__((sentinel(-1)));
int f2(int) __attribute__((sentinel));
int f3(const char *, ...) __attribute__((sentinel(f1)));
__attribute__((__int128__)) int j;
__attribute__((__foo__)) int k;
void f(bool b)
{
  x + 1;
  x + y;
  x - d;
  x / d;
  d / x;
  fz + x;
  x == d;
  x + 1.0iF;
  __builtin_va_arg (d, double);
  switch (b) { case true: case false: default: break; }
  switch (b) { case true: case false: break; }
  switch (b) { case true: default: break; }
}
