// flags: -Wall
void __attribute__((aligned(8), packed)) f1(void);
void __attribute__((aligned(8))) f2(void);
void __attribute__((packed)) f2(void);
int __attribute__((noreturn)) f3(void);
int __attribute__((warn_unused_result)) f3(void);
int __attribute__((noreturn, warn_unused_result)) f4(void);
void *__attribute__((noreturn)) f5(int);
void *__attribute__((alloc_align(1))) f5(int);
void __attribute__((noreturn)) f6(int);
void __attribute__((malloc)) f6(int);
void __attribute__((always_inline)) f7(void);
void __attribute__((noinline)) f7(void) { }
void __attribute__((always_inline)) f7(void);
int __attribute__((pure, noreturn)) f8(void);
void __attribute__((aligned(32))) f9(void);
void __attribute__((aligned(16))) f9(void);
void __attribute__((aligned(32), aligned(16))) f10(void);
extern int __attribute__((common)) v1;
extern int __attribute__((nocommon)) v1;
