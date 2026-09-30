// flags: -Wall -Wextra
int n = 3;
struct F { int x; int y[]; };
struct F f1 = { 1, { 2, 3 } };
struct F f2 = { 1 };
struct F f3 = { 1, 2 };
struct F f4 = { 1, { } };
struct G { int a; struct F f; };
struct G g1 = { 1, { 2, { 3 } } };
struct H { struct F f; int z; };
void fn(int k) {
  int v[k] = { 1 };
  int w[k] = { };
  int u[k][2] = { { 1 } };
  int x[n];
  int y[k] = 1;
  char s[k] = "a";
  int z[2] = { [k] = 1 };
  int q[3] = { [1 ... k] = 1 };
  struct F f = { 1, { 2 } };
  static struct F sf = { 1, { 2 } };
  struct F f5 = { 1 };
  struct F *p = &(struct F){ 1, { 2 } };
  int r = { k };
  int rr[2] = { k, k + 1 };
  static int st = k;
  static int st2[2] = { 1, k };
  static int *sp = &k;
  static int sn = n;
}
int vla_file[n];
int ar[n] = { 1 };
