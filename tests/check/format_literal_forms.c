// flags: -Wformat
const char *pf = "%d";
void f(void)
{
  __builtin_printf(pf, 1.0);
  __builtin_printf((const char *)L"x");
  __builtin_printf((const char *)"%d", 1.0);
  __builtin_printf(&"%d"[0], 1.0);
  __builtin_printf("%d" + 0, 1.0);
  __builtin_printf("%d%d" + 2, 1.0);
  __builtin_printf(1 ? "%d" : "%u", 1.0);
}
