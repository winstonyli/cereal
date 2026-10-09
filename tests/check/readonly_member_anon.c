// flags: -std=c11
/* gcc's readonly_error wording for a member: "member 'q' in read-only
 * object" when the object holding it is const (through anonymous members,
 * their qualifiers count, nested too), else "read-only member 'q'"; the
 * same for ++, --, compound assignment and asm outputs (which also catch a
 * record with a const member). */
struct A {
    int x;
    const struct { int q; };
    struct { const int r; };
    const struct { struct { int n; }; };
    struct { const struct { int z; } s; };
};
struct B { const int k; };
struct C { struct { int y; }; struct B b; };
void f(struct A *p, const struct A *cp, struct C *pc, const struct C *cpc,
       struct A a)
{
    p->q = 1; p->q++; --p->q; p->q += 2;
    p->r = 1; p->r++;
    p->n = 1; p->n--;
    p->s.z = 1;
    cp->x = 1; cp->q = 1; cp->r = 1; cp->n++;
    pc->b.k = 1; *pc = *pc;
    cpc->y = 1; cpc->b.k = 1;
    a.q = 1; (a).q -= 1; (a.q) = 2;
    __asm__ ("" : "=r"(a.q));
    __asm__ ("" : "=r"(a.r));
    __asm__ ("" : "=r"(cp->x));
    __asm__ ("" : "=r"(cp->q));
    __asm__ ("" : "=r"(*pc));
}
