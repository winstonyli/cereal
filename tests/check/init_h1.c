// flags: -Wall -Wextra
struct P { int x, y; };
int fn(void); int g;
static int v0[] = { .s = { { "ab", (struct P){1,2}, fn, g }, { 1 } } };
