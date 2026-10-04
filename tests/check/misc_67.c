typedef void (*fp)(void);
typedef void (*nrfp)(void) __attribute__((noreturn));
void (*nr2)(void) __attribute__((noreturn));
void __attribute__((noreturn)) (*nr3)(void);
void f1(nrfp);
void f2(fp x) { f1(x); }
void f3(fp x, nrfp z)
{
    nrfp y = x;
    y = x;
    nr2 = x;
    nr3 = x;
    x = z;
}
void f4(int a[], int *b[])
{
    __builtin_printf("%zu %zu\n", sizeof(*&a), sizeof(*&b));
}
int *f5(int x) { return __builtin_speculation_safe_value(x); }
