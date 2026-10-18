/* Variadic definitions and the va_list builtins. */
typedef __builtin_va_list va_list;
int sum(int n, ...)
{
    va_list ap, bp;
    int s = 0;
    double d;
    __builtin_va_start(ap, n);
    __builtin_va_copy(bp, ap);
    while (n--)
        s += __builtin_va_arg(ap, int);
    d = __builtin_va_arg(bp, double);
    __builtin_va_end(bp);
    __builtin_va_end(ap);
    return s + (int)d;
}
int vf(const char *f, va_list ap) { return __builtin_va_arg(ap, int); }
long ptrs(int n, ...)
{
    va_list ap;
    char *p;
    __builtin_va_start(ap, n);
    p = __builtin_va_arg(ap, char *);
    __builtin_va_end(ap);
    return p[0] + n;
}
void noargs(const char *fmt, ...) { }
