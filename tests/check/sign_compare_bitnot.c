// flags: -Wsign-compare
int t(unsigned char c, unsigned short s, signed char sc, unsigned u, int i, long long ll, unsigned char d) {
  int r = 0;
  r += ~c == 0;            /* 3 */
  r += 0 == ~c;
  r += ~c != 0;
  r += ~c < 5;
  r += ~c > 0;
  r += ~c == i;            /* 8 */
  r += ~c == u;
  r += ~c == d;
  r += ~c == 5;
  r += ~c == -5;
  r += ~c == -1;
  r += ~c == 0xffffff05u;
  r += ~s == 0;            /* 15 */
  r += ~s == d;
  r += ~sc == 0;
  r += ~u == 0;
  r += ~c == ~d;            /* 19 */
  r += ~c == ll;
  r += (char)~c == 0;
  r += (unsigned char)~c == 0;
  r += ~c == 256u;
  r += ~c == 0u;            /* 24 */
  r += !~c;
  r += ~c && 1;
  r += ~c ? 1 : 2;
  r += (~c) == 0;          /* 28 */
  r += ~(c) == 0;
  r += ~(unsigned char)u == 0;
  r += ~c == (short)0;
  r += ~c <= d;            /* 32 */
  return r;
}
typedef _Bool bool;
bool g2(unsigned char c, unsigned short s) {
  bool b;
  if (~c) b = 1;
  while (~c) break;
  for (; ~c; ) break;
  b = ~c;
  b = !~c;
  b = ~c || 1;
  b = 1 && ~c;
  b = (bool)~c;
  b = (_Bool)(~c);
  b = (~c) ? 1 : 0;
  b = !(~s);
  b = !!~c;
  bool x = ~c;
  do {} while (~c);
  return ~c;
}
