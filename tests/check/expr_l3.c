// flags: -Wall -Wextra
struct S { int a; const int b; };
void f(const struct S cs, struct S *ps)
{
    *ps += 1;
    cs += 1;
}
