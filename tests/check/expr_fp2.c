// flags: -Wall -Wextra
typedef void (*c1)(int, ...);
typedef void (*c2)(char *, ...);
typedef void (*c3)(...);
typedef void (*c4)(struct S, int, ...);
void a1(c1); void a2(c2); void a4(c4);
void f(void (*p)(void)) { a1(p); a2(p); a4(p); }
