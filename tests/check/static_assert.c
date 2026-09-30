_Static_assert(1, "ok");
_Static_assert(0, "fail");
_Static_assert(sizeof(int) == 4, "int size");
_Static_assert(sizeof(long) == 4, "long size");
int n = 2;
_Static_assert(n == 2, "not constant");
_Static_assert(1.5, "float");
struct S { _Static_assert(1, "in struct"); int x; };
void f(void) { _Static_assert(2 > 1, "in func"); _Static_assert(0, "in func fail"); }
_Static_assert((char)300 == 44, "trunc");
