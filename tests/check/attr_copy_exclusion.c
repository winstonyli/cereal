// flags: -Wall
/* copy brings along every exclusion-table attribute of the referenced function except the inline
 * family (gcc applies those by flags): the copied attribute meets the declaration's own. */
int h1(int) __attribute__((cold));
int h2(int) __attribute__((hot, copy(h1)));
int h3(int) __attribute__((copy(h1), hot));
int n1(int) __attribute__((noreturn));
int n2(int) __attribute__((pure, copy(n1)));
int n3(int) __attribute__((const, copy(n1)));
int m1(int) __attribute__((returns_twice));
int m2(int) __attribute__((noreturn, copy(m1)));
char *q1(int) __attribute__((malloc));
char *q2(int) __attribute__((pure, copy(q1)));
int w1(int) __attribute__((warn_unused_result));
int w2(int) __attribute__((noreturn, copy(w1)));
int a1(int) __attribute__((always_inline));
int a2(int) __attribute__((noinline, copy(a1)));
int i1(int) __attribute__((noinline));
int i2(int) __attribute__((always_inline, copy(i1)));
