// flags: -std=c23
struct B { int b : 3; } sb;
void f(void) {
    auto x = sb.b;
    __auto_type y = sb.b;
    auto;
    __auto_type;
    auto w = w;
}
__auto_type n = {1};
__auto_type *q = 0;
__auto_type r[2] = {1, 2};
struct S { int x; };
__auto_type u = (struct S){1};
int a, b;
__auto_type v = a + b;
