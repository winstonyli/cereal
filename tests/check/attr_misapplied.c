extern void __attribute__((dllimport, unk))
  f1();
void __attribute__((nosuch)) f2() { }
typedef int T __attribute__((noinline, weak, used));
struct S { char c __attribute__((noinline, used, weak)); } __attribute__((used));
int v __attribute__((noinline, used, weak));
int g(int p __attribute__((used, weak)))
{
  static int s __attribute__((noinline, used));
  auto int a __attribute__((used));
  return p;
}
