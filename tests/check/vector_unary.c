typedef int i4 __attribute__((vector_size(16)));
typedef float f4 __attribute__((vector_size(16)));
typedef long long l2 __attribute__((vector_size(16)));
i4 a, b; f4 f, g; l2 l; int n; float x; struct S {int q;} s;
void t(void)
{
  -a; +a; ~a; !a; -f; ~f; !f; ++a; a++; --f; f--; ~l;
  ~x; -s; +s; !s; ~s;
  a ? a : b;
  a ? 1 : 2;
  n ? a : b;
  n ? a : 1;
  n ? a : f;
  n ? f : g;
  a ? f : g;
  a ? a : f;
  a ? a : l;
  a ? 1 : a;
  a ? x : f;
  f ? a : b;
  s ? a : b;
  n ? a : s;
  a ? s : s;
  a ? (void)0 : (void)0;
  a == b ? a : b;
  a ? 1 : 2.0;
  a ? n : n;
}
