// flags: -Wall
void f1(void)
{
  struct T x;
  struct T y; y.a = 1;
  void z;
  int w[];
}
void g1(void){ struct T *p; struct T q; (void)p; }
void h1(void){ struct T x2 = {0}; struct U u3; u3.x; }
void f2(void)
{
  struct T x;
  struct T y; y.a = 1;
  struct T z; (void)z;
  struct T w; w = w;
  struct T v; int i = sizeof v;
  struct T *pp; (void)pp;
}
