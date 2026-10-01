char b[2] __attribute__((aligned(UND2)));
int z __attribute__((vector_size(UND6)));
struct S { int a __attribute__((aligned(UND5))); };
void f(void) { char a[16] __attribute__((aligned(UND))); }
int ok __attribute__((aligned(8)));
