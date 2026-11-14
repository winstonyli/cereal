// flags: -Wall -Wextra
int sum(const int *a, int n)
{
#pragma cereal requires n >= 0
#pragma cereal ensures n >= 0
  int s = 0;
  for (int i = 0; i < n; i++) {
#pragma cereal invariant 0 <= i && i <= n
    s += a[i];
  }
  return s;
}
int imp(int a, int b)
{
#pragma cereal requires a > 0 ==> b > 0
  return b;
}
int tern(int a, int b, int c)
{
#pragma cereal requires a ==> b ? c : a
#pragma cereal requires a ==> b ==> c
#pragma cereal requires a || b ==> c && a
  return a;
}
