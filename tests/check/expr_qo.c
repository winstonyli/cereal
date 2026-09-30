// flags: -Wall -Wextra
struct S { int a; };
void f(const int ci, volatile int vi, const float cf, const char cc, const struct S cs, const int *cp, int *const pc, const struct S *ps, const int ca[3], _Bool cb)
{
    int r; struct S s;
    r = cs + 1;
    r = ci + s;
    r = cf % 2;
    r = cp + cp;
    r = pc * 2;
    r = ps * 2;
    r = cc + s;
    r = vi + s;
    r = cb + s;
    r = cp * pc;
    r = ~cf;
    r = cf << 1;
    r = cs < 1;
    r = cs == cs;
    r = -cs;
    r = ci + cs;
    r = s * cs;
}
void g(const int ci, const struct S cs) {
    const int *p = &ci;
    r = p < cs;
}
