// Argument counts and kinds gcc checks itself for some built-ins.
#include <stdint.h>
static void *p;
static double *d;
static int r;
enum E { A };
void f(int n, float x, const int *cp, _Bool *bp, enum E *ep)
{
  p = __builtin_alloca_with_align(n, 6);
  p = __builtin_alloca_with_align(n, SIZE_MAX);
  p = __builtin_alloca_with_align(n, 64);
  r += __builtin_isnan(0);
  r += __builtin_isnan(x);
  r += __builtin_isgreater(0, 0);
  r += __builtin_isgreater(x, 0);
  r += __builtin_isgreater(&x, 0);
  r += __builtin_fpclassify(1, 2, n, 4, 5, x);
  r += __builtin_fpclassify(1, 2, 3, 4, 5, 6);
  d = __builtin_assume_aligned(p, n, p);
  d = __builtin_assume_aligned(p, n, 1, 2);
  r += __builtin_add_overflow(n, x, &r);
  r += __builtin_add_overflow(n, n, cp);
  r += __builtin_add_overflow(n, n, bp);
  r += __builtin_add_overflow(n, n, ep);
  r += __builtin_add_overflow_p(n, n, (_Bool)1);
  r += __builtin_add_overflow_p(n, n, 1.0);
  if (__builtin_constant_p()) r = 1;
  if (__builtin_constant_p(1, 2)) r = 2;
}
