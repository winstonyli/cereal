// flags: -std=c99 -Wc90-c99-compat
void f(int n, int a[n]) { n = 1; int i; long long l; }
void fn7 (int n, int a[n]);
void fn8 (int n) { int a[n]; }
long long l;
const const int i;
volatile volatile int v;
enum E { A };
struct S { int i; int a[3]; struct { int x; } in; };
struct B { int a:1; unsigned b:1; signed c:1; _Bool d:1; char e:1; short f:1; long g:1; long long h:1; enum E k:1; unsigned char m:2; signed int n:2; };
void f(void)
{
  struct S s = { .i = 1, .a[1] = 2, .in.x = 3 };
  int a[3] = { [1] = 2, [2] = 3 };
  int *p = (int[]){ 1, 2 };
  struct S *q = &(struct S){ 1 };
}
enum F { X, Y, };
enum G { Z, };
_Complex double cz = __builtin_complex(0.0, 0.0);
void lp(void) { for (int i = 0; i < 2; i++) ; }
