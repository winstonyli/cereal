#define cat(a,b) a##b
cat(.,.) /* expect: error */
cat(+,-) /* expect: error */
#define str(x) #x
#define f(x, y) x
f(1) /* expect: error */
f(1,2,3) /* expect: error */
#if 1/0 /* expect: error */
#endif
#if 1 +  /* expect: error */
#endif
#if 1.0 /* expect: error */
#endif
#ifdef /* expect: error */
#endif
#define defined /* expect: error */
#define bad(a, a) a /* expect: error */
#define bad2(a) #b /* expect: error */
#define bad3 ## x /* expect: error */
#include "does_not_exist.h" /* expect: error */
#elif 1 /* expect: error */
#endif /* expect: error */
#define V(...) __VA_ARGS__
#define W(x) __VA_ARGS__ /* expect: error */
#if 0
#garbage fine in skipped group
#endif
#garbage /* expect: error */
#error custom message /* expect: error */
#warning custom warning /* expect: pp-warning-directive */
#define X 1
#define X 2 /* expect: macro-redefined */
#undef __FILE__ /* expect: builtin-macro-redefined */
#if 0
#else
#else /* expect: error */
#endif
f( /* expect: error */
