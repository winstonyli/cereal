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
const int g = 10;
const int h = g + 1;
void ff(void) { int k = h << 31; (void)k; }
void f3(void) { const int a = 10; const int b = a + 1; int k = b << 31; (void)k; }
const int n = 3;
int arr[n];
int m[n + 1] = {0};
