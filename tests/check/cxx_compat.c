// flags: -Wc++-compat
enum E1 { A, B };
enum E2 { C, D };
struct S { enum E2 f : 2; int i; };
static void g(enum E1 e) { (void)e; }
enum E2 conv(enum E1 e) { return e; }
void h(void)
{
    enum E1 e1 = A;
    enum E2 e2 = e1;
    struct S s = { B };
    int x = 1;
    int *p = &x;
    void *vp = p;
    int *q = vp;
    struct S t;
    t.f = e1;
    e2 = x ? e1 : e2;
    e1++;
    g(e2);
    (void)q; (void)s; (void)t;
    (void)sizeof(struct T { int a; });
    (void)(struct U { int a; } *)0;
}
