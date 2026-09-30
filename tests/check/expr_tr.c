// flags: -Wall -Wextra
struct S { int a; int b[2]; };
int g(void);
int gv;
extern int ga[4];
struct S gs;
extern int weakv __attribute__((weak));
int wf(void) __attribute__((weak));
static int sf(void);
void f(int i, int *p, struct S *sp, int arr[3], struct S s)
{
    int r, a[3], *lp = &r;
    static int st;
    register int rg;
    r = !g;
    r = !a;
    r = !&i;
    r = !gv;
    r = !ga;
    r = !gs.b;
    r = !sp->b;
    r = a && i;
    r = i && a;
    r = a || i;
    r = g || i;
    r = i ? 1 : 2;
    r = a ? 1 : 2;
    r = g ? 1 : 2;
    r = &gv ? 1 : 2;
    r = gs.b ? 1 : 2;
    r = &gs.a ? 1 : 2;
    r = &gs ? 1 : 2;
    r = &a[1] ? 1 : 2;
    r = &a[1] == 0;
    r = &gs.a == 0;
    r = gs.b == 0;
    r = 0 == g;
    r = g != 0;
    r = g == (void *)0;
    r = (void *)g == 0;
    r = (long)g == 0;
    r = wf ? 1 : 2;
    r = &weakv ? 1 : 2;
    r = sf ? 1 : 2;
    r = &st ? 1 : 2;
    r = &s ? 1 : 2;
    r = &s.a ? 1 : 2;
    r = s.b ? 1 : 2;
    r = "abc" ? 1 : 2;
    r = "abc" == 0;
    r = &r == 0;
    r = lp == 0;
    r = a + 1 == 0;
    r = p + 1 == 0;
    r = (a + 1) ? 1 : 2;
    r = &gv + 1 == 0;
    r = &a[0] != 0;
    r = &*p == 0;
    r = sp->b == 0;
    r = &sp->b == 0;
    r = &sp->a == 0;
    r = &sp->b[1] == 0;
    r = &gs.b[1] == 0;
    r = __builtin_expect(g != 0, 1);
    r = a == a;
    r = a == p;
    r = a < p;
    r = a != arr;
    r = gs.b == a;
    r = a >= ga;
    r = "a" == "a";
    r = s.b == gs.b;
    r = s.b == (int *)0;
    if (a) {}
    if (g) {}
    while (g) {}
    for (;g;) {}
    do {} while (a);
    if (!a) {}
    if (g && i) {}
    if (a, i) {}
    if ((a)) {}
    if ((void *)a) {}
    if ((long)a) {}
    if (&a[0]) {}
    if (a + 0) {}
    if (g() && a) {}
}
