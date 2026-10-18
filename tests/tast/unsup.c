// T1 subset edges: each of these drops its whole unit with one reason.
struct S { int x; double d; };
struct S mk(void);
int rv(void) { return mk().x; }
double dm(struct S *s) { return s->d; }
int vla(int n) { int a[n]; return n; }
int cl(void) { return ((int[]){1})[0]; }
int sideef(int *p, int i) { return p[i++] += 1; }
int kr(a) int a; { return a; }
int f(void);
double fl(double d) { return d; }
int strs(void) { return sizeof("x"); }
int fcast(void) { return (int)1.5; }
int fi(void);
int ovr(void) { int a[2] = { [0] = fi(), [0] = 3 }; return a[0]; }
int ovs(void) { static int a[2] = { [0] = 1, [0] = 3 }; return a[0]; }
