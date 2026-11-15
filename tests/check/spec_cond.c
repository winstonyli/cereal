struct S { int x; };
int c1(struct S s, int n)
{
#pragma cereal requires s
  return n;
}
int c2(int n)
{
#pragma cereal requires (void)n
  return n;
}
int c3(int n)
{
#pragma cereal requires (n, n)
  return n;
}
int ok(int n, int *p)
{
#pragma cereal requires p && n > 0
#pragma cereal requires p ? n : 1
  return n;
}
