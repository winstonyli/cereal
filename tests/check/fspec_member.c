// flags: -std=c11
struct t { int a; inline void (*f)(void); };
struct u { int a; _Noreturn int b; };
int s1 = sizeof(inline int);
void g(inline int x);
