// T1 subset edges: each of these drops its whole unit with one reason.
struct S { int x; double d; };
struct S mk(void);
int rv(void) { return mk().x; }
double dm(struct S *s) { return s->d; }
int vla(int n) { int a[n]; return n; }
int cl(void) { return ((int[]){1})[0]; }
int sideef(int *p, int i) { return p[i++] += 1; }
int strinit(void) { char s[] = "x"; return s[0]; }
int kr(a) int a; { return a; }
int gi = 3;
int f(void);
int va(int, ...);
int callva(void) { return va(1, 2); }
int unproto();
int callup(void) { return unproto(1); }
double fl(double d) { return d; }
int strs(void) { return sizeof("x"); }
int stat(void) { static int n; return n; }
int brace(void) { int a[2] = {1, 2}; return a[0]; }
int fcast(void) { return (int)1.5; }
