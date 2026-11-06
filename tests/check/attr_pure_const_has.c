// flags: -Wall
/* __builtin_has_attribute after pure/const exclusion, redeclaration and copy */
int a(int) __attribute__((pure, const));
extern char a_pure[__builtin_has_attribute(a, pure) ? 1 : -1];
extern char a_const[__builtin_has_attribute(a, const) ? -1 : 1];
int b(int) __attribute__((pure));
int b(int) __attribute__((const));
extern char b_pure[__builtin_has_attribute(b, pure) ? 1 : -1];
extern char b_const[__builtin_has_attribute(b, const) ? -1 : 1];
int p(int) __attribute__((pure));
int q(int) __attribute__((const));
int c1(int) __attribute__((copy(p), copy(q)));
extern char c1_pure[__builtin_has_attribute(c1, pure) ? 1 : -1];
extern char c1_const[__builtin_has_attribute(c1, const) ? -1 : 1];
int c2(int) __attribute__((copy(p), const));
extern char c2_pure[__builtin_has_attribute(c2, pure) ? 1 : -1];
extern char c2_const[__builtin_has_attribute(c2, const) ? -1 : 1];
int c3(int) __attribute__((const, copy(p)));
extern char c3_const[__builtin_has_attribute(c3, const) ? 1 : -1];
extern char c3_pure[__builtin_has_attribute(c3, pure) ? -1 : 1];
int t(int) __attribute__((pure, const));
int c4(int) __attribute__((copy(t)));
extern char c4_pure[__builtin_has_attribute(c4, pure) ? 1 : -1];
extern char c4_const[__builtin_has_attribute(c4, const) ? -1 : 1];
