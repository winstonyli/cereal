/* TAST T2: pointers, arrays, decay, pointer arithmetic, function pointers. */
int g;
int arr[4];
int *gp;
const char *msg(void) { return "hi"; }
int *addr_of(int x) { int y = x; int *p = &y; return p; }
int deref(int *p) { return *p; }
void store(int *p, int v) { *p = v; }
int *adv(int *p) { return p + 2; }
int *adv2(int *p, long i) { return i + p; }
int *back(int *p, short i) { return p - i; }
long diff(int *p, int *q) { return p - q; }
char *cinc(char *p) { p++; return p; }
int *iinc(int *p) { ++p; p += 3; p -= 1; return p--; }
int first(int a[]) { return a[0]; }
int idx(int i) { return arr[i]; }
int rev(int i) { return i[arr]; }
int neg(int *p) { return p[-1]; }
int two(int a[3][2], int i, int j) { return a[i][j]; }
int *decay(void) { return arr; }
int (*whole(void))[4] { return &arr; }
int nullcmp(int *p) { return p == 0 || p != (int *)0; }
int ptrcmp(int *p, int *q, void *v) { return p < q && v == p && !p; }
int cond(int *p, int c) { return (c ? p : 0) != 0; }
_Bool tobool(int *p) { _Bool b = p; return b; }
long frompointer(int *p) { return (long)p; }
int *fromlong(long v) { return (int *)v; }
void *voidp(char *p) { return p + 1; }
int fp(int (*f)(int), int x) { return f(x) + (*f)(x); }
int call_named(int x);
int viaaddr(int x) { int (*f)(int) = call_named; return (&call_named)(x) + f(x); }
int volat(volatile int *p) { return *p; }
int sideeff(int *p, int i) { return p[i++] += 1; }
