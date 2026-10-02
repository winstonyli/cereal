// flags: -std=c99 -pedantic
#define L(fmt, ...) f(fmt, ##__VA_ARGS__)
#define N(a, ...) h(a, __VA_ARGS__)
int f(const char *, ...), g(int, ...), h(int, ...);
void t(void) { L("a"); L("a", 1); N(1); N(1, 2); }
