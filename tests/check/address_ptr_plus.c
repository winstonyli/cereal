// flags: -Wall
void T(int);
typedef unsigned long size_t;
extern int ai[4]; extern char ac[4]; extern long al[4];
int (*pa)[2]; int *p; char *pc; long *pl; 
void f(int i, unsigned u, long l, size_t z, short s, char c, int *q) {
  T(p + i == 0);
  T(p + u == 0);
  T(p + l == 0);
  T(p + z == 0);
  T(p + s == 0);
  T(p + c == 0);
  T(pc + i == 0);
  T(pc + z == 0);
  T(pc - u == 0);
  T(pl + i == 0);
  T(pl - l == 0);
  T(i + p == 0);
  T(p - z == 0);
  T(p + i + 1 == 0);
  T(p + 1 + i == 0);
  T(p + (i + 1) == 0);
  T(p + i * 2 == 0);
  T(&p[i + 1] == 0);
  T(ai + i == 0);
  T(ac + i == 0);
  T(al - i == 0);
  T(&ai[i] == 0);
  T(&ai[1] + i == 0);
  T(*pa + 1 == 0);
  T(*pa + i == 0);
  T(pa + 1 == 0);
  T(pa[1] == 0);
  T(&pa[0][i] == 0);
  T(&(*pa)[0] == 0);
  T(&*p == 0);
  T(&q[i] == 0);
  T(q + i == 0);
}
#define G(x, i) ((&x) + i)
int m_f (int *p, int i) { return G (*p, i) != 0; }
int m_g (int *p, int i) { return ((&*p) + i) != 0; }
int m_h (int *p, int i) { return (p + i) != 0; }
int b_f (void) { return __builtin_malloc != 0; }
int b_g (void) { return __builtin_abort != 0; }
int b_h (void) { return __builtin_strlen != 0; }
int b_k (void) { return __builtin_printf != 0; }
