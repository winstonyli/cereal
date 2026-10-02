// flags: -std=gnu99 -Wshadow=local
int g;
void f(int a, long b)
{
  int a2;
  {
    long a;
    int b;
    int a2;
  }
}
