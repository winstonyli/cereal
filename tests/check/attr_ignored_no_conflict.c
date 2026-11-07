// flags: -Wall
/* An attribute its handler ignores (malloc on a non-pointer, alloc_size/alloc_align on a non-pointer,
 * warn_unused_result on void) is not in gcc's list, so a later pure/const/noreturn does not conflict
 * with it. */
int f1(int) __attribute__((malloc, pure));
int f2(int) __attribute__((alloc_size(1), const));
int f3(int) __attribute__((alloc_align(1), noreturn));
void f4(void) __attribute__((malloc, pure));
void f5(void) __attribute__((warn_unused_result, noreturn));
_Noreturn int f6(int);
int f6(int) __attribute__((malloc, pure));
char *f7(int) __attribute__((malloc, pure));
/* an ignored noreturn does not make the definition noreturn */
int d1(int) __attribute__((pure));
__attribute__((noreturn)) int d1(int x) { return x; }
/* gcc says a conflict with an earlier declaration's warn_unused_result twice */
int w1(int) __attribute__((warn_unused_result));
int w1(int) __attribute__((noreturn));
