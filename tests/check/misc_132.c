// flags: -Wconversion
struct S { signed char x : 1; unsigned u : 3; long long l : 40; } s;
int bar;
typedef short V __attribute__((vector_size(16)));
const V cy;
enum E;
void f(int i) {
    s.x = f;
    s.u = f;
    s.l = f;
    s.x = bar ? 2 : 0;
    s.x = bar ? 0 : 2;
    s.u = bar ? 9 : 1;
    cy[i] = 1;
    (void)(enum E)i;
}
