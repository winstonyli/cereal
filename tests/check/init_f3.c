// flags: -Wall -Wextra
int i2 = 2147483647 + 1;
  static   int i3 = 2147483647 + 1;
int i4[2] = {
  1,
  2147483647 + 1,
};
struct S { int a, b; } s = { 1,
   2147483647 + 1 };
void f(void) {
  int l = 2147483647 + 1;
  static int m[] = { 1,
       2147483647 + 1 };
  int n = 2147483647 + 1, o = 1 - -2147483647 - 3;
}
int j1 = 1, j2 = 2147483647 + 1;
long k1 = 9223372036854775807 + 1;
unsigned k2 = 4294967295u + 1;
int k3 = -(-2147483647 - 1);
