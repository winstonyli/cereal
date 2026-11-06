// flags: -Wall
/* noreturn against pure/const, across redeclaration and with all three on one declaration */
_Noreturn int n1(void) __attribute__((const));
int n1(void) __attribute__((pure));
int n2(void) __attribute__((pure, const, noreturn));
extern char n2_pure[__builtin_has_attribute(n2, pure) ? 1 : -1];
extern char n2_const[__builtin_has_attribute(n2, const) ? -1 : 1];
extern char n2_noreturn[__builtin_has_attribute(n2, noreturn) ? -1 : 1];
int n3(void) __attribute__((const));
int n3(void) __attribute__((noreturn));
