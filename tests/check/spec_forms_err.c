typedef unsigned long size_t;
void v(int *p)
{
#pragma cereal ensures return == 0
}
int w(int *p, int n)
{
#pragma cereal requires return == 0
#pragma cereal requires p[0:n] + 1
#pragma cereal requires -p[0:n]
#pragma cereal requires $len(n) > 0
#pragma cereal requires $len(p, n) > 0
#pragma cereal requires $len p
#pragma cereal requires p[0:] == p
#pragma cereal requires p[n:n] == p[1.5:2]
#pragma cereal requires p[n:n] > p
  return n;
}
int x(int q)
{
  int a[2] = {q, q};
  return a[0:1] == a;
}
