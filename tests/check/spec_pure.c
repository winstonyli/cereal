int f(int n);
int g(int n, int *p)
{
#pragma cereal requires n++ > 0
#pragma cereal requires --n > 0
#pragma cereal requires (n = 1)
#pragma cereal requires f(n)
#pragma cereal requires ({ n; })
#pragma cereal requires n > 0 && p[n] == *p && sizeof(n) == 4
  return n;
}
