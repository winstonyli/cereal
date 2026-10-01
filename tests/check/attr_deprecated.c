// flags: -std=c99
void f(void) __attribute__((unavailable("gone")));
int g __attribute__((deprecated("old")));
typedef int T __attribute__((deprecated));
void h(void) { f(); g = 1; T x = 0; (void)x; }
