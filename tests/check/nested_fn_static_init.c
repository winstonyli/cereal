struct S { void (*f)(int); };
void foo(void)
{
  void bar1(int v) { (void)v; }
  static struct S s1 = { bar1 };
  static void (*p)(int) = &bar1;
  void (*ok)(int) = bar1;
}
