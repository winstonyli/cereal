/* ## and variadic macros: instantiated like the others */
#define ATT(n) (FLT_ ## n)
#define CAT(a, b) a ## b
#define SUM(...) (__VA_ARGS__)
#define FIRST(a, ...) a
#define TWO(a, b, ...) (a + b)
#define FLT_BITS 24
#define LEVEL_3 3
#if ATT(BITS) > 20
int a;                  /* FLT_BITS: 24 > 20 */
#endif
#if CAT(LEVEL_, 3) == 3
int b;                  /* LEVEL_3 */
#endif
#if CAT(LEVEL_, LIVE) > 1
int c;                  /* LEVEL_LIVE: an initial value */
#endif
#if SUM(1, 2) == 2
int d;                  /* (1, 2): comma operator, not modeled */
#endif
#if FIRST(0, 1, 2)
int e;
#endif
#if TWO(1, 2) == 3
int f;                  /* the variadic argument may be absent */
#endif
#if FIRST()
int g;
#endif
#if CAT(1, 2) == 12
int h;                  /* a number with a number: not an identifier */
#endif
#if P * Q == 12
int i;                  /* opaque; the host values it from -D (paste.flags) */
#endif
#if P * Q == 13
int j;
#endif
