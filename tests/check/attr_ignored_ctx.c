// flags: -std=gnu99 -Wall -Wno-unused
typedef int T1 __attribute__((visibility("hidden")));
typedef int T2 __attribute__((retain, nodirect_extern_access, externally_visible));
typedef union { int *a; long *b; } U __attribute__((transparent_union));
int g1 __attribute__((noreturn, hot, flatten));
int g2 __attribute__((mode(QI)));
int g3 __attribute__((interrupt, sseregparm));
void f(int p1 __attribute__((visibility("default"))),
       int p2 __attribute__((nonnull)),
       int p3 __attribute__((used)));
struct S {
    int a __attribute__((alias("x"), common));
    int b __attribute__((format(printf, 1, 2)));
};
enum E { A __attribute__((visibility("hidden"))), B __attribute__((malloc)) };
struct __attribute__((visibility("hidden"))) V { int x; };
void __attribute__((malloc)) m1(void);
int m2(void) __attribute__((malloc));
int m3 __attribute__((malloc));
void h(void)
{
    int l1 __attribute__((retain, hot, visibility("hidden")));
    static int l2 __attribute__((pure));
    extern int l3 __attribute__((volatile));
}
typedef char T3 __attribute__((nonstring));
char n1[4] __attribute__((nonstring));
char *n2[2] __attribute__((nonstring));
unsigned char n3[2][3] __attribute__((nonstring));
int n4[4] __attribute__((nonstring));
int *n5 __attribute__((nonstring));
struct N { char a[3] __attribute__((nonstring)); long b __attribute__((nonstring)); };
