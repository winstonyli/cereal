// flags: -Wall
__attribute__ ((section ("s1"))) void f1 (void);
__attribute__ ((section ("s2"))) void f1 (void);
__attribute__ ((section ("s3"), section ("s4"))) void f2 (void);
__attribute__ ((section ("s5"))) __attribute ((section ("s6"))) void f3 (void);
int v1 __attribute__((section("a")));
int v1 __attribute__((section("b")));
int v2 __attribute__((section(3)));
int v3 __attribute__((section));
int v4 __attribute__((section("a"), section("a")));
void g(void) {
  int l __attribute__((section("x")));
  static int s __attribute__((section("y")));
}
typedef int T __attribute__((section("t")));
struct S { int m __attribute__((section("z"))); };
void f4(int p __attribute__((section("q"))));
int v5 __attribute__((section("a"))) = 1;
extern int v6 __attribute__((section("a")));
int v6 __attribute__((section("a")));
void f1(void) {}
void fa(void) __attribute__((section("a")));
void fa(void) __attribute__((section("b")));
__attribute__((section("a"))) void fb(void) {}
void fb(void) __attribute__((section("c")));
void h2(void)
{
    extern int le __attribute__((section("x")));
    static int ok __attribute__((section("x")));
    (void)ok;
}
