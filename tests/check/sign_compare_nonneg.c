// flags: -Wsign-compare
int tf;
int f(unsigned u, unsigned char b, unsigned short s, int i, int x)
{
  int r = 0;
  r += u > b * 100;
  r += u > s * 100000;
  r += u > s * s;
  r += u > b + 100;
  r += u > s + 65535;
  r += u > i * 100;
  r += u > 100 + b;
  r += u > b + s;
  r += u < ((x = 22) / 33);
  r += u < ((x = 22) % -33);
  r += u > ({tf; 64;});
  r += u > ({tf; tf ? 64 : -1;});
  r += u > (tf, 5);
  r += u > (tf, -5);
  return r;
}
