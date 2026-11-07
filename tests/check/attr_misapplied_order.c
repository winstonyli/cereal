void cl(int *);
typedef int T1 __attribute__((weak, noinline, aligned(4), used));
typedef int T2 __attribute__((leaf, noinline, weak));
struct S {
  char a __attribute__((used, leaf, weak));
  char b __attribute__((noinline, pure, packed));
  char c __attribute__((cleanup(cl), const, used));
};
int v1 __attribute__((noinline, leaf, packed));
extern int v2 __attribute__((alias("v1"), pure, cleanup(cl)));
static int v3 __attribute__((cold, noinline, used, error("x")));
int f1(int p __attribute__((leaf, used, noinline)));
int g(int p __attribute__((used, pure, weak)))
{
  static int s __attribute__((cold, noinline, warning("w")));
  auto int a __attribute__((leaf, used, packed));
  extern int e __attribute__((pure, used, cleanup(cl)));
  extern int h(void) __attribute__((alias("g"), pure));
  return p;
}
struct __attribute__((packed)) PK { char c; int i; };
extern int cp __attribute__((copy((struct PK *)0), pure));
void fcopy(void) __attribute__((copy((struct PK *)0), used));
struct E { void d[1] __attribute__((packed)); };
