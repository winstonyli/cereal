int kr(n)
  int n;
{
#pragma cereal requires n > 0
  return n;
}
int nest(int n)
{
  int in(int m)
  {
#pragma cereal requires m > 0
    return m;
  }
  return in(n);
}
int pr(int n)
{
  _Pragma("cereal requires n > 0")
  return n;
}
int mac(int *p)
{
#pragma cereal requires p != NULL
  return *p;
}
int side(int n)
{
#pragma cereal requires n++ > 0
  return n;
}
int last(int n)
{
  return n;
}
#pragma cereal requires 1
