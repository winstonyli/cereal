// T1 subset edges: each of these drops its whole unit with one reason.
struct S { int x; double d; };
struct S mk(void);
int rv(void) { return mk().x; }
__int128 dm(struct S *s) { return s->x; }
int vla(int n) { int a[n]; return n; }
int kr(a) int a; { return a; }
int f(void);
_Complex double fl(_Complex double d) { return d; }
int strs(void) { return sizeof("x"); }
int fcast(void) { return (int)1.5; }
int ovs(void) { static int a[2] = { [0] = 1, [0] = 3 }; return a[0]; }
struct A { long a; };
struct SQ { struct A t; } sqv = { (struct A){ 2 } };
struct A sav = (struct A){ 3 };
int scl(void) { static struct A s = (struct A){ 5 }; return s.a; }
