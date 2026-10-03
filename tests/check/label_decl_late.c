// flags: -std=c99 -pedantic
void f(void)
{
  int x;
  __label__ a;
  x = 1;
  __label__ b;
  a: b: return;
}
void g(void)
{
  { __label__ q; goto q; q:; }
  ;
  __label__ z;
}
void h(void) { for (;;) { __label__ k; goto k; k:; } }
