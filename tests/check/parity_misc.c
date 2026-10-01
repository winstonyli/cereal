/* misc gcc-parity cases: implicit-int redefinition, volatile return, offsetof ->, NPC comma, VM arrays, gnu_inline, address compare */
void foo (int, int[*]);
foo (int x, int y) { return x + y; }
extern int xxx (void);
volatile extern int xxx (void);
struct S { int c; struct { float f; } sa[2]; };
char a[__builtin_offsetof (struct S, sa->f) == __builtin_offsetof (struct S, sa[0].f) ? 1 : -1];
int *p; long *q;
void g (void)
{
  q = (1 ? p : (void *)(0, 0L));
  p = (1 ? p : (void *)(1 ? 0 : (0, 0)));
}
void h (int i)
{
  typedef int (*Ti)[i];
  Ti (*qf)[2];
  static int x[sizeof (*qf)];
  (void)x;
}
#define inline __attribute__((gnu_inline)) inline
extern int func4 (void);
extern inline int func4 (void) { return 4; }
int func4 (void) { return 5; }
int obj;
int cmp = (&obj >= 0);
struct T { int z };
