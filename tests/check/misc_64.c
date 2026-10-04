// flags: -Wall -Wextra -Wmemset-elt-size -Wdate-time
typedef int T;
typedef _Bool bool;
extern void *memset(void *, int, unsigned long);
extern void bar(void);
enum E { A, B, C __attribute__((unused)), D };
int arr[10];
char buf[16];
const char *when = __TIME__;
static int nr(void) __attribute__((noreturn));
static int nr(void) { for (;;) bar(); }

int f(int a, int b, bool bb, enum E e)
{
  int u, v, w, r = 0;
  _Complex int cz;
  memset(buf, sizeof buf, 0);
  memset(arr, 0, 10);
  r += sizeof arr / sizeof(short);
  r += sizeof arr / (sizeof(short));
  r += ~bb;
  r += ~(a == b);
  r += ~(bar(), a < b);
  r += (a < b ? : 1);
  r += (bb ? : 1);
  r += !a > 0;
  r += !a == 0;
  r += (nr == nr);
  switch (e) {
  case A: case B: case D: break;
  }
  ({ u = a; });
  v = (w = a, b);
  __real__ cz = 1;
  return r;
}
