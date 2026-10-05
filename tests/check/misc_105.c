// flags: -std=gnu99
/* Attribute group: used in a parameter pointer declarator; common/nocommon
 * and tls_model through copy; alloc_size on a function typedef; optimize
 * option spelling. */
#define U __attribute__ ((used))
void p1 (int (*U) (void));
void p2 (int (*U bar) (void));
void p3 (int *U q);
void p4 (int U q);
void p5 (U int q);
#define A(...) __attribute__ ((alloc_size (__VA_ARGS__)))
typedef A (2, 3) char* F (int, int, int);
typedef A (1) char* F (int, int, int);
typedef A (1) char* G (int, int, int);
typedef A (2) char* G (int, int, int);
typedef A (1) char* H (int, int, int);
typedef char* H (int, int, int);
__attribute__ ((common)) int cv;
__attribute__ ((nocommon)) double nv;
__attribute__ ((copy (cv), copy (nv))) long x1;
__attribute__ ((copy (nv), copy (cv))) long x2;
__attribute__ ((copy (cv))) __attribute__ ((nocommon)) long x3;
__attribute__ ((nocommon)) __attribute__ ((copy (cv))) long x4;
__attribute__ ((copy (cv, nv))) long x5;
__attribute__ ((tls_model ("global-dynamic"))) __thread int tt;
int nt __attribute__ ((tls_model ("global-dynamic")));
__attribute__ ((copy (tt))) extern int al1;
__attribute__ ((copy (tt))) extern __thread int al2;
extern int al3 __attribute__ ((copy (tt)));
__attribute__ ((copy (tt))) int al4;
void f (void) { __attribute__ ((copy (tt))) static int al5; }
int __attribute__((optimize("no-lto"))) a(void){return 0;}
int __attribute__((optimize("lto"))) b(void){return 0;}
int __attribute__((optimize("-fno-lto"))) c(void){return 0;}
int __attribute__((optimize("no-inline"))) d(void){return 0;}
int __attribute__((optimize("foo"))) e(void){return 0;}
int __attribute__((optimize("no-foo"))) f(void){return 0;}
int __attribute__((optimize("-flto"))) g(void){return 0;}
int __attribute__((optimize("no-fat-lto-objects"))) h(void){return 0;}
int __attribute__((optimize("whole-program"))) i(void){return 0;}
int __attribute__((optimize("no-use-linker-plugin"))) j(void){return 0;}
int __attribute__((optimize("Ofast"))) k(void){return 0;}
int __attribute__((optimize("2"))) l(void){return 0;}
int __attribute__((optimize("O3"))) m(void){return 0;}
