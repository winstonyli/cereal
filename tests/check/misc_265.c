// flags: -Wformat
int a = (1 << 31) - 1;
int b(void) { int x = (1 << 31) - 1; return x; }
int c(void) { int x; x = (1 << 31) - 1; return x; }
int d(void) { return (1 << 31) - 1; }
int e(void) { return (2147483647 + 1) - 1; }
short f(void) { short w = ((short)1 << 31) - 1; return w; }
int printf(const char *, ...);
void g(void) { printf("abc %d\n"); }
#line 100
void h(void) { printf("abc %d\n"); }
