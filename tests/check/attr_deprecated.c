void f(void) __attribute__((unavailable("gone")));
int g __attribute__((deprecated("old")));
typedef int T __attribute__((deprecated));
void h(void) { f(); g = 1; T x = 0; (void)x; }
struct S { int a __attribute__((deprecated("f"))); int b __attribute__((unavailable)); };
struct __attribute__((deprecated)) D { int x; };
struct D *pd;
int fld(struct S *s)
{
  return s->a + s->b;
}
