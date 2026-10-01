// flags: -Wconversion
short s; int a, b; unsigned u; unsigned char uc; char c; long long l; unsigned long ul; float f; double d; long double ld;
void fs(short); void fu(unsigned); void ff(float);
short r1(int x) { return x; }
unsigned r2(int x) { return x - 1; }
float r3(double x) { return x; }
void h(void) {
  s += a; s += 1; s -= s; s *= 2; s <<= 1; uc += u; u += a; u += -1; a += u; f += d; f += a; a += f; c += 1;
  u = u + a; u = a + 1; u = a + 1u; a = a + u; a = u * 2;
  u = a & b; u = a | 1; a = u & 0x7fffffff;
  if (a < u) ; if (a == u) ; if (u > -1) ;
  l = a + u; ul = a; ul = l; l = ul;
  a = l >> 3; a = (int)l >> 3; s = c; c = s; uc = c; c = uc;
  f = l; d = f; f = ld; a = ld; ld = a;
  fs(a); fu(a); ff(d); fs(c); fu(u + a);
  u = a ? u : a; a = a ? 1.5 : 2;
  s = a++; s = (short)a; s = a, s = b;
  uc = ~uc; uc = -uc; u = -a; u = ~a; a = -u;
  a = u / 2; a = u % 3; u = a / 2; u = a % 3;
  a = 1u << 3; u = 1 << 3; s = 1 << 20; uc = 1 << 7;
  u = 0x80000000; a = 0x80000000; a = 4294967295; uc = 'a'; uc = -1; c = 200;
  f = 1.1; f = 1.5; f = 3; d = 1.1f; f = 1e300; a = 1.5; u = 1.5; a = 1e10;
}
