_Noreturn void d(void) __attribute__((noreturn, const));
int f1(int) __attribute__((pure, alloc_size(1)));
void f3(void) __attribute__((pure, const));
int f4(int) __attribute__((pure, malloc));
void f5(void) __attribute__((noreturn, pure));
void f6(void) __attribute__((const, noreturn));
