// flags: -Wtraditional
struct S { int a; };
typedef struct S T;
void f(int i)
{
    int a[2] = { 1, 2 };
    int b = 1, c[2] = { 1, 2 };
      char s[] = "abc";
    struct S x = { 1 }, y = { 2 };
    T z = { 3 };
    const struct S w = { 4 };
    struct { int q; } an = { 5 };
    struct S *p = 0, v = { 6 };
    for (i = 0; i < 1; i++) {
    int d[1] = { 1 };
    }
    typeof(a) e = { 1, 2 };
    struct S arr[1] = { { 1 } };
    register struct S r = { 7 };
}
