#include <stdlib.h>
#include <string.h>
#define R __attribute__((scalar_storage_order("big-endian")))
struct R A { int i; };
struct R B { char c; };
struct R C { struct A a[1]; };
struct R D { struct A a; };
struct R E { int x[2]; };
struct R F { struct B b; };
void f(void *v)
{
  struct A *a = malloc(4);
  struct B *b = malloc(4);
  struct C *c = malloc(4);
  struct D *d = malloc(4);
  struct E *e = malloc(4);
  struct F *ff = malloc(4);
  a = v; b = v; c = v; d = v; e = v; ff = v;
  memset(a, 0, 4); memset(b, 0, 4); memset(c, 0, 4);memset(d, 0, 4);memset(e, 0, 4);memset(ff,0,4);
  memcpy(v, a, 4);
  v = a; v = b; v = c; v = d; v = e; v = ff;
}
