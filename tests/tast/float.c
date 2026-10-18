/* T4a: float, double, long double as scalars: constants, arithmetic,
 * comparisons, conversions, ++/--, compound assignment, calls, ?:. */
int printf(const char *, ...);
double dadd(double a, double b) { return a + b; }
float fmul(float a, float b) { return a * b; }
long double lsub(long double a, long double b) { return a - b; }
double neg(double a) { return -a; }
double mixed(float f, double d, int i) { return f + d * i; }
double lit(void) { return 1.5; }
float flit(void) { return 0.1f; }
long double llit(void) { return 3.25L; }
double fromint(void) { return 3; }
float fromneg(void) { return -2; }
double nz(void) { return -0.0; }
int trunc_(double d) { return (int)d; }
unsigned long ul(double d) { return (unsigned long)d; }
double fromu(unsigned long u) { return (double)u; }
double widen(float f) { return f; }
float narrow(double d) { return (float)d; }
long double lw(double d) { return d; }
int lt(double a, double b) { return a < b; }
int eq(float a, float b) { return a == b; }
int truth(double d) { return d ? 1 : 2; }
int not_(double d) { return !d; }
int both(double a, float b) { return a && b; }
_Bool tobool(double d) { return d; }
double sel(int c, float a, double b) { return c ? a : b; }
double elvis(double a, double b) { return a ?: b; }
void incs(double *p, float *q) { (*p)++; --*q; }
double postinc(double d) { d++; return d; }
void ops(double *p, float *q, int i) { *p += 1; *q *= 2; *p /= i; *p -= *q; }
int toint(float *p) { int i = 1; i += *p; return i; }
void call(float f, double d, long double l) { printf("%f %f %Lf", f, d, l); }
double callp(double (*fp)(double, double)) { return fp(1, 2.5); }
struct S { float f; double d; long double l; };
double fld(struct S *s) { return s->f + s->d + s->l; }
double arr(double *a, int i) { return a[i] * 2.0; }
