// flags: -Wall -Wextra
typedef void (*fp)(void);
struct S {int a;};
void f(float fl, struct S s, char *cp, void *vp, int i)
{
    cp = (int *)fl;
    vp = (fp)fl;
    cp = (fp)fl;
    i = (fp)fl;
    cp = (int *)s;
    vp = (fp)s;
    i = (int *)s;
    (fp)fl;
    (void)(fp)fl;
    fl = (int)cp + (fp)fl;
}
