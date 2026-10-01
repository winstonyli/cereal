// flags: -Wconversion
int si; unsigned ui; unsigned char uc; signed char sc; long long sl; unsigned long long ul; float f; double d; int a;
void h(void) {
  si = 3000000000u;
  ui = -1;
  sl = 10000000000000000000ull;
  ul = -5;
  uc = -1;
  sc = 200;
  sc = 200u;
  uc = 255;
  f = 0.1;
  f = 16777217u;
  d = 3.1f;
  f = 1.0e40;
  si = 2147483648.0;
  a = a ? -1 : 0;
  ui = a ? -1 : 0;
  uc = a ? 1 : 300;
  sc = a ? 1u : 3000000000u;
  ui = a ? -1.5 : 2;
}
