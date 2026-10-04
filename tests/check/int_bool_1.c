// flags: -Wint-in-bool-context
#define M 8
#define MUL(a,b) ((a)*(b))
#define SH(a,b) ((a)<<(b))
int f(int a, int b, unsigned u, long l, _Bool x, char c) {
  int r = 0;
  if (a * b) r++;
  if (a * 2) r++;
  if (2 * a) r++;
  if (a * M) r++;
  if (MUL(a,b)) r++;
  if (a << b) r++;
  if (a << 1) r++;
  if (1 << a) r++;
  if (SH(a,b)) r++;
  if (u * u) r++;
  if (l * l) r++;
  if (c * c) r++;
  if (x << 1) r++;
  if (a ? 1 : 2) r++;
  if (a ? 0 : 2) r++;
  if (a ? 1 : 0) r++;
  if (a ? 1 : 1) r++;
  if (a ? b : 2) r++;
  if (a ? 2 : b) r++;
  if (!(a * b)) r++;
  if (!(a << b)) r++;
  if (!(a ? 1 : 2)) r++;
  if (a && (b * 2)) r++;
  if (a || (b << 2)) r++;
  if ((a * b) && a) r++;
  if ((a * b) || a) r++;
  while (a * b) r++;
  for (;a * b;) r++;
  do r++; while (a<<b);
  r = (a * b) ? 1 : 2;
  r = (a ? 1 : 2) ? 1 : 2;
  r = !(a * 1.0);
  if ((a ? 4 : 2) & 1) r++;
  if (a * b == 1) r++;
  if (a * 3 * b) r++;
  if (a * (b * 3)) r++;
  if (a << 1 << b) r++;
  if (-(a*b)) r++;
  if ((a*b, a)) r++;
  if (a ? 1 : (b ? 2 : 3)) r++;
  if ((a ? 1 : 2), a) r++;
  if (a ? (b?1:2) : 3) r++;
  if (a ? 0 : (b?1:2)) r++;
  return r;
}
