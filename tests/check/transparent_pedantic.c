// flags: -std=gnu99 -pedantic
typedef union { int *p; long *q; } U __attribute__((transparent_union));
void f(U);
void f(int *);
void g(int *);
void g(U);
void h(U);
void h(U);
void bl(void) { void f(int *); void k(U); void k(long *); }
