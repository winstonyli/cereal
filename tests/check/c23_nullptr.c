// flags: -std=c23
#include <stddef.h>
nullptr_t a = nullptr;
int *p = nullptr;
int *q = a;
_Bool b = nullptr;
int i = nullptr;
int j = (int)nullptr;
void *v = (void *)nullptr;
int c1 = nullptr == p;
int c2 = a == nullptr;
int c3 = a == p;
int c4 = a == 0;
int c5 = !nullptr;
int c6 = nullptr && 1;
int *c7 = 1 ? nullptr : p;
nullptr_t c8 = 1 ? nullptr : a;
int c9 = _Generic(nullptr, nullptr_t: 1, void *: 2, default: 3);
typeof(nullptr) t1;
int s1 = sizeof(nullptr_t), s2 = _Alignof(nullptr_t);
int c10 = nullptr + 1;
int c11 = a < p;
int c12 = nullptr;
nullptr_t c13 = 0;
nullptr_t c14 = p;
nullptr_t c15 = (nullptr_t)p;
nullptr_t c16 = (nullptr_t)0;
void f(int *);
void g(void) { f(nullptr); f(a); nullptr_t z = nullptr; z = nullptr; if (z) {} if (nullptr) {} a++; *nullptr; (void)&nullptr; }
void h(int, ...);
void k(void) { h(1, nullptr); }
constexpr nullptr_t cn = nullptr;
constexpr int *cp = nullptr;
static_assert(nullptr == nullptr);
static_assert(!nullptr);
int arr[nullptr == 0 ? 1 : 2];
nullptr_t *pn = &a;
struct S { nullptr_t m; } st = { nullptr };
int unr(int x) { if (x) return 1; unreachable(); }
