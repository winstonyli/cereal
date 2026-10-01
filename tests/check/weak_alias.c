static void a(void) __attribute__((weak));
static int b __attribute__((weak));
void f(void) {
  __attribute__((weak)) void g(void) {}
  static int h __attribute__((weak));
  int i __attribute__((weak));
  extern int j __attribute__((weak));
  g();
}
extern inline int k(void) __attribute__((weak));
static int m(void);
int m(void) __attribute__((weak));
int n(void);
static int n(void) __attribute__((weak));

void t1(void) __attribute__((alias("t0")));
void t1(void) {}
extern int v1 __attribute__((alias("v0")));
int v1 = 3;
void t2(void) {}
void t2(void) __attribute__((alias("t0")));
static void t3(void) __attribute__((weakref("bar")));
static void t3(void) {}
void i1(void) __attribute__((ifunc("r")));
void i1(void) {}
extern int v0;
