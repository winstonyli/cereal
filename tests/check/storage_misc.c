register int a;
auto int b;
void f(register int x, auto int y, static int z) { }
void g(void) { extern int e = 1; }
void h(void) { static int s; register int r; auto int a2; typedef int T; extern int x; }
__thread int t1;
void i(void) { __thread int t2; static __thread int t3; extern __thread int t4; }
_Thread_local int t5;
int j(void) { extern void jj(void) { } }
_Noreturn void nr(void);
_Noreturn int nr2;
inline int il;
inline void il2(void);
void k(void) { inline int q; }
