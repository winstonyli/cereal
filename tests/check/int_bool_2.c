// flags: -Wint-in-bool-context
#define BOOLM(x) if (x) r++
#define W(a,b) (a) * (b)
#define ISM if (a * b) r++
int f(int a, int b, unsigned u, long l, unsigned long ul, _Bool x, char c, unsigned char uc, short s, double d) {
  int r = 0;
  if (u << b) r++;               /* 6 */
  if (u << 1) r++;
  if (l << b) r++;
  if (ul << b) r++;
  if (c << b) r++;
  if (uc << b) r++;
  if (s << b) r++;
  if (a << u) r++;
  if (x << b) r++;
  if (uc * uc) r++;              /* 15 */
  if (s * s) r++;
  if (d * d) r++;
  if (a * 0) r++;
  if (a * 1) r++;
  if (0 * a) r++;
  if (a * x) r++;
  if (x * x) r++;
  BOOLM(a * b);                  /* 23 */
  BOOLM(a);
  ISM;
  if (W(a,b)) r++;
  _Bool y = a * b;               /* 27 */
  y = a * b;
  y = (_Bool)(a * b);
  y = (_Bool)(a << b);
  y = a ? 1 : 2;
  r = (long)(a * b) ? 1 : 0;     /* 32 */
  if ((char)(a * b)) r++;
  if ((a * b) != 0) r++;
  if ((a << b) & 1) r++;
  if (~(a * b)) r++;
  if (+(a * b)) r++;
  if (((a * b))) r++;
  if ((a * b) + 1) r++;
  if (a * b || d) r++;
  if (a ? x : 2) r++;
  if (a ? d : 2) r++;
  if (a ? 2.0 : 3) r++;
  if (a ? 'a' : 'b') r++;
  if (a ? 4 : 0) r++;
  if (a ? 4 : 5) r++;
  if (a ? 2 : 2) r++;
  if (a ? 2L : 3) r++;
  if (a ? 0 : 1) r++;
  if (a ? 1 : x) r++;
  if (a ? u : 2) r++;
  r = (a ? 2 : 3) && b;
  r = !(a ? 2 : 3);
  r = (a ? 3 : 4) || b;
  while ((a ? 2 : 3)) r++;
  r = (a = 3) ? 2 : 3;
  if (r++ ? 7 : 9) r++;
  return r;
}
