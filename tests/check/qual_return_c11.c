// flags: -std=c11 -pedantic-errors -Wignored-qualifiers
int f1(void);
const int f1(void);
volatile int f1(void) { return 0; }
int *restrict f2(void) { return 0; }
int *f2(void);
const volatile void f4(void) {}
void f4(void);
_Atomic int f5(void);
int f5(void);
restrict int f7(void);
typedef void FT(void);
FT *restrict f8(void);
