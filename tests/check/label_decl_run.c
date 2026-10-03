// flags: -std=c99 -pedantic
int f(void)
{
  __label__ a, b;
  __label__ c;
  int r = 0;
  goto a;
a: goto b;
b: goto c;
c: return r;
}
int g(void)
{
  __label__ x;
  goto x;
x: return 1;
}
