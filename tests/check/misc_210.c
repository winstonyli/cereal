// flags: -pedantic
#define bad1 ## owt
#define bad2 owt ##
#define bad3(x) ## x
#define bad4(x, y) x ##
#define bad5(x) # y
#define a(x, y...) foo(x, ##y)
int foo(int, ...);
void f(void) { int g = a(1); (void)g; }
#if (1, 2) != 2 || (2, 1) != 1
#endif
#if 1 ? 2 : 3 , 0 /* c */
#endif
#if 1 ,
#endif
#if 0 - (-9223372036854775807 - 1) /* c */
#endif
#if -1 << 1 != -2 || 3 << 62 != 0
#endif
#if ab
#endif
#undef X y
#ifdef X z
#endif
#endif x
