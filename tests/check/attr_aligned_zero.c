// flags: -Wall
__attribute__((aligned(0))) void f0(void) { }
void f1(void) __attribute__((aligned(0)));
int v __attribute__((aligned(0)));
typedef int T __attribute__((aligned(0)));
struct S { int a __attribute__((aligned(0))); } __attribute__((aligned(0)));
void g(void) { int l __attribute__((aligned(0))); }
