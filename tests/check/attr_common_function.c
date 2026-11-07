// flags: -Wall
/* common and nocommon apply to variables only: on a function declaration or definition they are
 * ignored (gcc prints one "ignored" line each, in list order). */
int c1(void) __attribute__((common));
int c2(void) __attribute__((nocommon));
int c3(void) __attribute__((common, nocommon));
int c4(void);
int c4(void) __attribute__((common));
int c5(void) __attribute__((common));
int c5(void) { return 0; }
__attribute__((nocommon)) int c6(void) { return 0; }
int c7(int) __attribute__((const, common));
void c8(void) __attribute__((pure, common));
char *c9(void) __attribute__((common)) __attribute__((nocommon));
int v1 __attribute__((common));
int v2 __attribute__((nocommon));
