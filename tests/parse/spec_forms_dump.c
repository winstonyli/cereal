int f(int *p, int n)
{
#pragma cereal requires $len(p) >= n && n >= 0 ==> p[0:n] == p[1:n + 1]
#pragma cereal ensures return > 0
  return n;
}
