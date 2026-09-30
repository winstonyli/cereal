// flags: -Wall -Wextra
struct S { int x; } s = { 1, 2 };
struct S s2 = { 1, 2 };
int d[1] = { 1, 2 };
void f(void){ int e[1] = {1,2}; }
