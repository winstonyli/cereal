/* #if arithmetic is unsigned when either operand is */
#if -1 < 0U
int a;                  /* -1 converts to UINTMAX_MAX: false */
#endif
#if ((0x7fffffffffffffff * 2UL + 1UL) >> 63) == 1
int b;                  /* constant, unsigned shift */
#endif
#if ((P * 2UL + 1UL) >> 63) == 1
int c;                  /* opaque; the host values it (unsigned.flags) */
#endif
#if (P * 2 + 1) >> 63 == 1
int d;                  /* signed: overflow wraps negative, arithmetic shift */
#endif
