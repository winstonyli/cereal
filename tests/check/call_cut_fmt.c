// flags: -Wunused-variable -Wformat
int printf(const char *, ...);
void f(int i)
{
  printf ("%" PRIfoo "\n", i);
  printf ("%" PRIfoo "\n", i);
}
