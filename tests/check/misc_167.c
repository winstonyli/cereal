// flags: -Wmisleading-indentation
void foo(int);
int a, b;
void t1(void)
{
  if (a) {
    foo (1);
  } else if (b)
         foo (2);
         foo (3);
}
void t2(void)
{
  if (a)
    foo (1);
  else if (b)
         foo (2);
         foo (3);
}
void t3(void)
{
  if (a) {
    foo (1);
  } else if (b)
    foo (2);
    foo (3);
}
void t4(void)
{
  if (a) {
    foo (1);
  }
  else if (b)
         foo (2);
         foo (3);
}
