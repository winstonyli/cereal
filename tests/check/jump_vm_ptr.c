// flags: -std=gnu99 -pedantic-errors
void f(int n) { goto a; { int (**b)[n]; a: 0; } }
void g(int n) { goto a; { typedef int (*b)[n], (*c)[n]; a: 0; } }
