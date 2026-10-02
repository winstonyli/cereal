// flags: -std=c99
void f(const int *p, const char *q, const long *l, const int (*pa)[3], const int *const *pp,
       int i, int j, unsigned u, unsigned long ul, short s)
{
  const int arr2[5] = {0};
  p[0] = 1; p[3] = 1; p[-1] = 1; p[i] = 1; p[i + 1] = 1; p[u] = 1; p[ul] = 1; p[s] = 1;
  q[0] = 1; q[2] = 1; q[i] = 1; q[i + j] = 1; q[1 + i] = 1; q[i + 1] = 1; q[-i] = 1;
  l[i] = 1; l[2] = 1;
  pa[1][2] = 1; pa[i][j] = 1; (*pa)[1] = 1;
  pp[1][i] = 1; pp[i][1] = 1;
  arr2[0] = 1; arr2[i] = 1;
  *p = 1;
}
