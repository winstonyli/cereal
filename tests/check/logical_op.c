// flags: -Wlogical-op
extern int x, y; extern unsigned u; extern char ch; extern int *p; extern double d; extern int g(void);
enum E { E0, E1, E2 };
#define M 5
void f1(void) {
  if (x < 0 && x > 10) {}
  if (x < 5 || x > 3) {}
  if (x < 5 && x > 3) {}
  if (x == 1 && x == 2) {}
  if (x != 1 || x != 2) {}
  if (u < 0 || u >= 0) {}
  if (x && !x) {}
  if (x || !x) {}
  if (x >= 0 || x < 0) {}
  if (x == 1 || x != 1) {}
  if (x > 1 && x < 1) {}
  if (p && !p) {}
  if (p == 0 || p != 0) {}
  if (ch == 3 && ch == 4) {}
  if (ch == 300 || ch != 300) {}
  if (x + 1 > 3 && x + 1 < 2) {}
  if (d > 1 && d < 0) {}
  if (d || !d) {}
  if (x == E1 && x == E2) {}
  if ((x & 1) && 4) {}
  if ((x & 1) || 4) {}
  if (x && 2) {}
  if (x && E2) {}
  if (x && 1) {}
  if (x && 0) {}
  if (x && M) {}
  if (x && (1+1)) {}
  if (x && (char)3) {}
  if (x && 2u) {}
  if (!x && 2) {}
  if ((x > 1) && 2) {}
  if (x && (y > 1) && 2) {}
  if (u && 0x100) {}
  if (p && 2) {}
  if (d && 2) {}
  if (x && 2.0) {}
  if (g() && 2) {}
  if (x && -1) {}
  if (x && x == 1) {}
  if (x == 1 && x) {}
  if (x != 0 && x == 3) {}
  if (x > 3 && !x) {}
  if (u > 3 || u <= 3) {}
  if (u > 3 || u < 3) {}
  if (x >= 3 || x <= 3) {}
  if (x && y && !y) {}
  if (x && (y || !y)) {}
  if ((x && y) || !(x && y)) {}
  int z = x && !x;
  z = (x < 0) || (x >= 0);
}
extern volatile int v; struct S { int a; } s, *ps; extern int arr[4]; extern double e;
void f2(void) {
  if (x == 1 && x == 1) {}
  if (x < y && x < y) {}
  if (x < y && y > x) {}
  if (x < y && !(x < y)) {}
  if (x < y || x >= y) {}
  if (d > 1 && d > 1) {}
  if (d && d) {}
  if (v && v) {}
  if (x + y && y + x) {}
  if (x - y && y - x) {}
  if (arr[x] && arr[x]) {}
  if (ps->a && ps->a) {}
  if ((u > 2) && (u > 2)) {}
  if (x == 1 || 1) {}
  if (x && x && x) {}
  if (x || x || y) {}
  if (x || y || x) {}
  if (x & 1 && x & 1) {}
  if (x ? 1 : 2 && x ? 1 : 2) {}
  if ((x ? y : 2) && (x ? y : 2)) {}
  if (ch && (int)ch) {}
  if (u && (int)u) {}
  if ((long)x && x) {}
  if ((short)x && (short)x) {}
  if (d == d && d == d) {}
  if (x * 2 && x << 1) {}
  if (-x && -x) {}
  if (~x && ~x) {}
  if (&x && &x) {}
  if (x == y || y == x) {}
  if (x != y && y != x) {}
  if (x + 1 > 3 && 3 < x + 1) {}
  if (x > 3 && 3 < x) {}
  if (x > 3 && 4 <= x) {}
  if (x >= 2 && x <= 2) {}
  if (x > 3 || x < 4) {}
  if (p == 0 && p == 0) {}
  if (x == 0 && !x) {}
  if (!x || x == 0) {}
  if (x == 3 && 3 == x) {}
  _Bool b1 = x && x;
  int r = x || x;
  while (x && x) {}
  for (; x && x;) {}
  r = (x && x) ? 1 : 0;
  r = x == 1 && x != 1;
  if ((x && x) && y) {}
}
extern int *q; extern _Bool b; extern unsigned char uc; extern long l; extern float fl;
void f3(void) {
  if (p && p) {}
  if (p || !p) {}
  if (p && q) {}
  if (b && b) {}
  if (b || !b) {}
  if (uc > 255 || uc < 256) {}
  if (uc < 10 && uc > 20) {}
  if (uc == 1 || uc != 1) {}
  if (l < 0 || l >= 0) {}
  if ((x & 1) && !(x & 1)) {}
  if (x < 3 && x >= 3) {}
  if (fl && fl) {}
  if (fl > 1 || fl <= 1) {}
  if (x == 2 && x == 2 + 1) {}
  if (x < 1 || x > 0) {}
  if (x <= 1 && x > 1) {}
  if (x + 1 < 3 && x + 1 > 5) {}
  if (x < 3 && (unsigned)x > 5) {}
  if (p == q && p != q) {}
  if (p == q || p != q) {}
  if (x == y && x == y) {}
  if (x && 2 && y) {}
  if (2 && x) {}
  if (x && sizeof x) {}
  if (x || y == 3 || 7) {}
  if (b && 2) {}
  if (x == 5 && (x > 7)) {}
  if (x == 5 || (x != 5)) {}
  int r = x && 2;
  r = (x ^ y) && 8;
  r = (x == y) && 8;
  r = -x && 3;
  r = x++ && 4;
  r = x && 0x7fffffff;
  r = x && (2, 3);
  r = x && ((void)0, 3);
}
extern int *p; extern float fl; extern int x;
void f4(void) {
  if (p && p != 0) {}
  if (p != 0 && p) {}
  if (p && p) {}
  if (!p && !p) {}
  if (p == 0 && !p) {}
  if (p != 0 && p != 0) {}
  if (fl && fl != 0) {}
  if (!fl && !fl) {}
  if (fl && !fl) {}
  if (fl == 0 && !fl) {}
  if (p && (p != 0)) {}
  if ((p) && p) {}
  if (x && x != 0) {}
  if (p && p != (void *)0) {}
  if (p && p != (int *)0) {}
  if (x && x != (void *)0) {}
  if (p || !p) {}
  if (p && (void*)p) {}
}
int pure(int) __attribute__((const)); extern int x;
void f5(void) { if (pure(x) && pure(x)) {} if (pure(x) && pure(x + 1)) {} }
