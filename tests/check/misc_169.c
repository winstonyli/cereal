// flags: -Wmisleading-indentation
void foo(int);
int a, b;
void u1(void)
{
  if (b);
  { foo (2); }
}
void u2(void)
{
  while (b);
  { foo (2); }
}
void u3(void)
{
  if (a)
    foo (1);
  else if (b);
  { foo (2); }
}
void u4(void)
{
  if (a)
    foo (1);
  else
    while (b);
  { foo (2); }
}
void u5(void)
{
  for (;;);
  { foo (2); }
}
