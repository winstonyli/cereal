// flags: -std=c11 -pedantic-errors
struct s2 { struct { int a; }; };
union u2 { union { int a; }; };
void f(void)
{
  int *p = &(_Alignas(16) int){0};
  (void)p;
  __extension__ _Static_assert(1);
}
