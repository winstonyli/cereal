// flags: -Wparentheses
int foo (int);
_Bool d;
int a, b, c;

int bar (int x, int y, int z)
{
  foo (!x & y);
  foo (!x | y);
  foo (!x & 2);
  foo (!1 & 2);
  foo (!x & (y + z));
  foo (!x | ~y);
  foo (!x & (y < z));
  foo (!x | (y && z));
  foo (!x & !y);
  foo ((!x) & y);
  foo (!(x & y));
  foo ((y + z) & !x);
  d = a = b;
  d = (a = b);
  c = a = b;
  return 0;
}
