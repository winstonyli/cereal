int f(int n)
{
#pragma cereal requires ({ int t = n; t > 0; })
  return n;
}
