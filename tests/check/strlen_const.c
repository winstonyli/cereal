// flags: -std=c99 -Wall
extern __SIZE_TYPE__ strlen(const char *);
struct A0 { char n, a[0]; };
struct A1 { char n, a[1]; };
struct Ax { char n, a[]; };
const struct A0 a0 = { };
const struct A1 a1 = { 0 };
const struct Ax ax = { 0 };
const char c[8] = "abc";
const char d[3] = "abc";
static const char e[3] = "abc";
char nc[4] = "ab";
void sink(unsigned);
#define T(x) sink(__builtin_strlen(x))
void f(int i)
{
  T("abc" + 3);
  T("abc" + 4);
  T("abc" - 1);
  T(c + 8);
  T(c + 3);
  T(&c[9]);
  T(c - 1);
  T(d);
  T(d + 1);
  T(d + 3);
  T(nc + 9);
  T(a0.a);
  T(a0.a - 1);
  T(a1.a);
  T(a1.a + 1);
  T(ax.a + 9);
  T(&*d);
  T((d));
  sink(strlen(e));
  sink(strlen(i ? d : "x"));
  sink(__builtin_strlen("ab\0c" + 5));
  sink(__builtin_strlen("ab\0c" + i));
}
