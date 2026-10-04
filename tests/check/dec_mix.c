// flags: -std=c99 -Wall
_Decimal32 a; _Decimal64 x; _Decimal128 y; double d; float f; int i; long double ld; _Complex double cd;
void g(void)
{
  x + 2.0;
  x <= 2.0;
  x == d;
  x * f;
  x + i;
  x + a;
  y - x;
  i ? x : d;
  i ? x : i;
  i ? x : a;
  x += d;
  d += x;
  x = d;
  x + ld;
  x + cd;
  -x;
  x && d;
  x / 1.0f;
}
