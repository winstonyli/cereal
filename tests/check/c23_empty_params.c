// flags: -std=c23 -Wstrict-prototypes -Wold-style-definition
int f();
int f(void);
int g() { return 1; }
int h(a, b) int a; int b; { return a + b; }
int (*fp)() = g;
int (*fq)(int) = g;
void u(void)
{
    f(1);
    g(2);
    f();
    fp(3);
    int (*x)(void) = fp;
    (void)x;
}
int k(int (*)());
int k(int (*)(void));
int m(int (*)(int));
int m(int (*)());
typedef int T();
T tt;
int tt(int);
