// flags: -Wdeclaration-after-statement -Werror=implicit
#if 9223372036854775808LL
#endif
enum E {};
void f(void)
{
  __label__ a;
  int i = 0;
  i++;
  int j = i;
  a: ;
  int k = j;
  { i = k; }
  int m = g(i);
}
