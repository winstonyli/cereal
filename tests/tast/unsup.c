// T1 subset edges: each of these drops its whole unit with one reason.
extern int (*gfp)(void);
int ind(void) { return gfp(); }
int kr(a) int a; { return a; }
int ptr(int *p) { return 1; }
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
