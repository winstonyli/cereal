// flags: -O -Wduplicated-branches
/* -O folds reads of const integer variables with constant initializers */
void f(int i, int *p)
{
  const int a = 10;
  const unsigned long b = sizeof (int) * 8 - 1;
  const volatile int v = 3;
  const int n = a;
  int k1 = a << b;
  int k2 = 10 << b;
  int k3 = v << 40;
  int k4 = n << 31;
  if (i > 10)
    *p = a * 2 + 1;
  else
    *p = 21;
  *p = (long) &a != 0;
}
