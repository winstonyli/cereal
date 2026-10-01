typedef int i4 __attribute__((vector_size(16)));
typedef unsigned u4 __attribute__((vector_size(16)));
typedef short s8 __attribute__((vector_size(16)));
typedef unsigned char uc16 __attribute__((vector_size(16)));
typedef float f4 __attribute__((vector_size(16)));
typedef double d2 __attribute__((vector_size(16)));
typedef long long l2 __attribute__((vector_size(16)));
i4 a; u4 b; s8 s; uc16 c; f4 f; d2 d; l2 l; i4 i2v; short sh; long long ll; int n; double dd;
struct S { int q; } st;
enum E { A } e;
void ok(void) {
  a + b; a + 1; s + -1; c + 255; f + 16777216; f + 1.5; d + 1.1f; a << ll; n << a;
  a == b; f < f; a == 1;
}
void bad(void) {
  a + 1.0;                       /* cannot convert */
  a % 1.5;                       /* invalid operands, real scalar */
  s + 65535;                     /* truncation (constant out of range) */
  s + n;                         /* truncation (non-constant) */
  f + 1.1;                       /* truncation (inexact) */
  f + n;                         /* int does not fit a float lane */
  d + ll;
  a + ll;
  ll << a;                       /* a scalar shifted by a vector converts */
  a + f;                         /* different element types */
  a + s;
  f % f; f & n; d | d; f << 1;   /* integer-only operators */
  a + e; a + (_Bool)1; a + st; a + &n;
  a == f; a == s; a == dd; s == n;
  a && n; n && a; a || a; n || st; st && n;
  if (a) ;
}
