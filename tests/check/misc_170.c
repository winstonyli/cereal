// flags: -Wmisleading-indentation
void foo(int);
int a, b;
void t1(void)
{
  if (a)
    foo (1);
  else if (b);
  foo (2);
}
void t2(void)
{
  if (a)
    foo (1);
  else if (b);
    foo (2);
}
void t3(void)
{
  if (a) {
    foo (1);
  }
  else if (b);
  { foo (2); }
}
void t4(void)
{
  if (b);
  foo (2);
}
void t5(void)
{
  if (a) foo(1); else if (b);
  foo (2);
}
void t6(void)
{
  if (a) foo(1);
  else
    if (b);
  foo (2);
}
