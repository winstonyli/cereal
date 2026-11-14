int f(int n)
{
#pragma cereal requires zz > 0
  return n;
}
int g(int n)
{
  n++;
#pragma cereal requires n > 0
  return n;
}
#pragma cereal requires 1
struct S {
#pragma cereal requires 1
  int x;
};
int h(int n)
{
  while (n)
#pragma cereal invariant n > 0
    n--;
  if (n) {
#pragma cereal invariant n > 0
    n++;
  }
  return n;
}
int i2(int a, int b)
{
#pragma cereal requires a == > b
  return a;
}
struct P { int x; };
int j(struct P q, int a)
{
#pragma cereal requires q ==> a
#pragma cereal requires a ==> q
#pragma cereal requires
#pragma cereal requries a
#pragma cereal ensures a a
  return a;
}
