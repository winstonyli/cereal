#define A __attribute__((aligned(1 << 29)))
typedef A char T1;
typedef char T2 A;
A char v1;
char v2 A;
struct S1 { A char f; };
struct A S2 { char f; };
struct S3 { char f; } A;
void f(void) { A char loc; static A char sl; }
void g(A char p);
A void h(void);
union U { char x A; };
typedef struct { char c; } A T4;
