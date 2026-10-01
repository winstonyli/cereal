// flags: -Wall
/* pure/const exclusion, attribute copying (pure, const, nonnull) */
__attribute__((const)) int a(void);
__attribute__((pure)) int b(void);
__attribute__((const, pure)) int c(void);
__attribute__((pure, const)) int d(void);
__attribute__((const)) int e(void);
__attribute__((pure)) int e(void);
__attribute__((pure)) int f(void);
__attribute__((const)) int f(void);
__attribute__((copy(a), copy(b))) int g(void);
__attribute__((pure, copy(a))) int h(void);
int __attribute__((const, pure)) c1(void);
extern int c3(void) __attribute__((const, pure));
int c5(void) __attribute__((const)),
   c6(void) __attribute__((pure, const));
int c8(void) __attribute__((const));
int
c8(void) __attribute__((pure));

__attribute__((nonnull)) void *t(void *p);
__attribute__((copy(t))) void *u(void *);
__attribute__((nonnull)) void *v(void *p) { return p; }
__attribute__((alias("v"), copy(v))) void *w(void *);
void k(void)
{
    u(0);
    v(0);
    w(0);
}
