int f(int n)
{
#pragma cereal requires n > 0
  for (int i = 0; i < n; i++) {
#pragma cereal invariant i >= 0
  }
  return n;
}
