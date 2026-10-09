// flags: -std=c11
/* C11 features are not pedantic warnings in C11; others still are. */
typedef int T;
typedef int T;
_Static_assert(__STDC_VERSION__ == 201112L, "c11");
struct S { int a; struct { int b; }; };
_Noreturn void f(void);
int g(void) { return _Generic(1, int: 1, default: 2) + _Alignof(int); }
_Atomic int x;
int a[0];
