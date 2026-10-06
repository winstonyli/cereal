// flags: -Wcast-align=strict -Wunused-local-typedefs
typedef struct { char x; } c;
typedef struct __attribute__((aligned(16))) { char x; } d;
typedef int f1(long); typedef int f2(void *); typedef void f4(); typedef void f5(void);
typedef long (*fl)(int);
int f(long);
char *x; c *y; d *z; short *sp; int *ip; long long *lp; void *vp; double *dp;
void foo(void)
{
  y = (c *)x; z = (d *)x;
  ip = (int *)x; ip = (int *)sp; sp = (short *)ip; lp = (long long *)ip;
  ip = (int *)vp; ip = (int *)(char *)ip; dp = (double *)ip;
  (void)(f2 *)f; (void)(f4 *)f; (void)(f5 *)f; (void)(fl)f; (void)(f1 *)f; (void)(void(*)(void))f;
  (void)(void *)f;
  (void)(int(*)(long, int))f;
}
void lt(void) { typedef int u1 __attribute__((used)); typedef int u2; }
