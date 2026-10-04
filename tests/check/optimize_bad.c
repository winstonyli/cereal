#pragma GCC push_options
#pragma GCC optimize ("-fno-lto")
int
f
(void)
{ return 0; }
static int p1(void), p2(void);
int v = 1;
#pragma GCC pop_options
int g(void) { return 0; }
int a1(void) __attribute__((optimize("-fno-lto")));
__attribute__((optimize("-march=x"))) int a2(void) { return 0; }
#pragma GCC optimize ("-fno-lto")
#pragma GCC reset_options
int k(void){return 0;}
