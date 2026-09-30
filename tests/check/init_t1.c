// flags: -Wall -Wextra
int y;
int a = y + 1;
int b[2] = { 1, y };
struct S { int x; int z; } s = { y, 2, 3 };
int c = { 1, 2 };
int d[2] = { 1, 2, 3 };
char e[2] = "abc";
char f[3] = "abc";
int g = {{1}};
struct S h = { 1 };
void fn(void) { int v = y; int w[3] = { y, [1]=2, [1]=3 }; struct S q = {.x=1, .x=2}; }
