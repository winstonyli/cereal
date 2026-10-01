int i;
void __GIMPLE foo()
{
  i = 1;
}
unsigned int __GIMPLE (ssa,startwith("x")) f(int a)
{
  int b;
  b = a;
  return b;
}
int k;
