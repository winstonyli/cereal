// flags: -Wall
/* pure/const through copy(): both orders, with noreturn, and a source whose own pure was excluded */
int a(void) __attribute__((const));
int b(void) __attribute__((pure));
int c(void) __attribute__((copy(a)));
int d(void) __attribute__((copy(a), pure));
int e(void) __attribute__((pure, copy(a)));
int f(void) __attribute__((const, copy(b)));
int g(void) __attribute__((noreturn, copy(b)));
int h(void) __attribute__((copy(b), noreturn));
int x(void) __attribute__((const, pure));
int i(void) __attribute__((copy(x)));
int j(void) __attribute__((copy(x), pure));
int k(void) __attribute__((copy(a), copy(b)));
