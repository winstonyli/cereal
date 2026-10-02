// flags: -Wall
/* inline after noinline and the converse; warn_unused_result on void */
void __attribute__((noinline)) f(void);
inline void f(void);
inline void g(void);
void __attribute__((noinline)) g(void);
void __attribute__((warn_unused_result)) h(void);
int __attribute__((warn_unused_result)) k(void);
void *__attribute__((alloc_size(1))) a(int);
void *__attribute__((noreturn)) a(int);
inline int __attribute__((aligned(8))) m(int);
int __attribute__((cold)) m(int);
inline int __attribute__((aligned(4))) m(int);
