// flags: -std=gnu99 -pedantic-errors
int f(int);
int n;
void t(int (*fp)(int))
{
  int a = __builtin_choose_expr(1, 2, "x");
  char *p = __builtin_choose_expr(0, 2, "x");
  int b = __builtin_choose_expr(n, 1, 2);
  int c = __builtin_choose_expr(1, 2);
  int d = __builtin_call_with_static_chain(f(1), fp);
  int e = __builtin_call_with_static_chain(n, fp);
  int g = __builtin_call_with_static_chain(f(1), n);
  (void)a; (void)p; (void)b; (void)c; (void)d; (void)e; (void)g;
}
