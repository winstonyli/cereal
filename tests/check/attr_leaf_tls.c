// flags: -Wall -Wno-unused
int v __attribute__((tls_model("global-dynamic")));
__thread int t __attribute__((tls_model("initial-exec")));
static int s __attribute__((tls_model("bogus")));
__thread int u __attribute__((tls_model("bogus")));
void f(void) { static int l __attribute__((tls_model("local-exec"))); (void)l; }
void (*fp)(int *__attribute__((leaf)) p);
void g(int *p __attribute__((leaf)));
void q1(int *p __attribute__((leaf)));
void q2(int *p __attribute__((leaf))) { (void)p; }
static void q3(int *p __attribute__((leaf)));
void (*r1)(int *__attribute__((leaf)) p);
void (*r2)(int p __attribute__((leaf)));
struct S2 { int a __attribute__((leaf)); void (*f)(int p __attribute__((leaf))); };
typedef void (*T2)(int p __attribute__((leaf)));
void h2(void) { int l __attribute__((leaf)); void (*q)(int __attribute__((leaf))); (void)l; (void)q; }
int w1 __attribute__((leaf));
int *__attribute__((leaf)) y1;
void p1(int *__attribute__((leaf)) p);
struct S3 { int *__attribute__((leaf)) a; };
void h3(void) { int *__attribute__((leaf)) l; (void)l; }
int *__attribute__((hot)) y2;
void p2(int *__attribute__((hot)) p);
