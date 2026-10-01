void cl(int *);
struct u1 { int a; };
typedef struct u1 T1 __attribute__((packed));
typedef int T2 __attribute__((packed));
struct S {
  unsigned char f[1] __attribute__((packed));
  char c __attribute__((packed));
  int i __attribute__((packed));
  int ia[2] __attribute__((packed));
  struct u1 s __attribute__((packed));
  char ca[3][2] __attribute__((packed));
  int *p __attribute__((packed));
};
int v __attribute__((packed));
void fn(void) __attribute__((packed));
typedef int Ti __attribute__((alias("x")));
extern int vb __attribute__((alias("v")));
void g1(void) { extern void bar() __attribute__((alias("BAR"))); extern int w __attribute__((alias("v"))); }
void g2(int p __attribute__((alias("v"))));
typedef int Tw __attribute__((weakref("U")));
static int wv __attribute__((weakref("U")));
void g3(int p __attribute__((weakref("U"))));
int ev __attribute__((error("foo")));
void ef(void) __attribute__((warning("w")));
void g4(void) { static int s1 __attribute__((cleanup(cl))); int a1 __attribute__((cleanup(cl))); extern int e1 __attribute__((cleanup(cl))); }
int fv __attribute__((cleanup(cl)));
void g5(int p __attribute__((cleanup(cl))));
void g6(void) __attribute__((cleanup(cl)));
