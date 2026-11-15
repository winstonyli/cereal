int a1(int n)
#pragma cereal requires n > 0
{
  return n;
}
int a2(int n)
{
  if (n)
#pragma cereal requires n > 0
    n++;
  return n;
}
int a3(int n)
{
l:
#pragma cereal requires n > 0
  n++;
  if (n < 3) goto l;
  return n;
}
int a4(int n)
{
  for (int i = 0;
#pragma cereal requires n > 0
       i < n; i++)
    n--;
  return n;
}
int a5(int n)
{
  switch (n) {
#pragma cereal requires n > 0
  case 1: n++;
  }
  return n;
}
int a6(int n)
{
  while (n) {
#pragma GCC ivdep
#pragma cereal invariant n > 0
    n--;
  }
  return n;
}
int a7(int n)
{
#pragma GCC unroll 2
#pragma cereal requires n > 0
  for (;n;) n--;
  return n;
}
int ok(int n)
{
#pragma cereal requires n > 0
#pragma cereal ensures n > 0
  while (n) {
#pragma cereal invariant n >= 0
    n--;
  }
  return n;
}
