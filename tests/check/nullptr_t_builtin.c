// flags: -std=c17
nullptr_t y;
int *f(int *p) { p = nullptr; p = nullptr_x; return p; }
void g(void) { nullptr_t z; (void)z; }
typedef int nullptr_t;
nullptr_t k = 1;
typedef long nullptr_t;
