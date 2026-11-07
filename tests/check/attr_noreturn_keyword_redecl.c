// flags: -Wall
/* _Noreturn is a declaration specifier, not an attribute: a later declaration's pure, const or malloc
 * does not conflict with it (gcc is silent), though __builtin_has_attribute sees noreturn. */
_Noreturn int g(void);
int g(void) __attribute__((pure));
_Noreturn int h(void);
int h(void) __attribute__((malloc));
_Noreturn int k(void);
int k(void) __attribute__((const));
_Noreturn int m(void) __attribute__((noreturn));
int m(void) __attribute__((pure));
_Static_assert(__builtin_has_attribute(g, noreturn), "keyword is a noreturn function");
