// flags: -Wtraditional-conversion
#include <stdbool.h>
enum E { A, B };
enum F { C = -1 };
void fb(_Bool);
void fe(enum E);
void fc(char);
void fi(int);
void fu(unsigned);
void fd(double);
void ff(float);
void fcd(_Complex double);
void fl(long);
struct S { void (*p)(short); } s;
enum E e; char c; short sh; unsigned short us; unsigned u; int i; long l; float f; double d;
_Bool b;
void g(void)
{
  fb(i); fb(c); fb(b);
  fe(e); fe(i); fe(u);
  fc(c); fc(i); fc(1); fc(sh);
  fi(c); fi(us); fi(sh); fi(u); fi(e); fi(b); fi(1u); fi(~0u);
  fu(i); fu(c); fu(-1); fu(1);
  fd(f); fd(d); fd(i); fd(l);
  ff(f); ff(d); ff(i);
  fcd(d); fcd(i); fcd(f);
  fl(i); fl(u); fl(l);
  s.p(i); s.p(sh); s.p(us);
  fi(1.0);
}
