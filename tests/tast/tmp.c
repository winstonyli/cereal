/* T5: a compound lvalue with a side effect is computed once, into a tmp. */
int f(void);
int *g(void);
int a[8], b[8];
struct S { int v[4]; int w; unsigned bf : 3; } s;
struct S *sp(void);
void inc(void) { a[f()]++; }
void dec(void) { --a[f()]; }
void deref(void) { (*g())--; }
void comp(void) { sp()->v[f()] += 2; }
void rhs(void) { *g() += *g(); }
void nest(void) { a[b[f()]++]++; }
int val(void) { return a[f()]++ + 1; }
void ptr(char **pp, int (*q)(void)) { pp[q()] += 1; }
void flt(double *d, int (*q)(void)) { d[q()] *= 2.5; }
void plain(int i) { a[i]++; a[i + 1] += 2; }
void bits(void) { sp()->bf += 1; }
volatile int vv;
void vol(volatile int *p) { *p += 1; vv += 1; }
