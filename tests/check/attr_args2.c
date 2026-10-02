// flags: -std=gnu99
// simd and zero_call_used_regs attribute arguments; returns_nonnull on a non-pointer function
int f(void) __attribute__((simd("notinbranch","inbranch")));
int g(void) __attribute__((simd(1)));
int h(void) __attribute__((simd("bug")));
int h2(void) __attribute__((simd("inbranch")));
int result __attribute__ ((zero_call_used_regs("all")));
int __attribute__ ((zero_call_used_regs("gpr-arg-all"))) foo1 (int x) {return 0;}
int __attribute__ ((zero_call_used_regs(1))) foo2 (int x) {return 0;}
int __attribute__ ((zero_call_used_regs())) foo3 (int x) {return 0;}
int __attribute__ ((zero_call_used_regs("skip"))) foo4 (int x) {return 0;}
int __attribute__ ((zero_call_used_regs("used-gpr","x"))) foo5 (int x) {return 0;}
int __attribute__ ((zero_call_used_regs)) foo6 (int x) {return 0;}
struct S { int z __attribute__ ((zero_call_used_regs("all")));};
typedef int T __attribute__ ((zero_call_used_regs("all")));
extern int rn() __attribute__((returns_nonnull));
