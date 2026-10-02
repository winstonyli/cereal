struct __attribute__((deprecated)) S1 { int a; };
struct __attribute__((deprecated)) S2 { int a; } z2;
struct S3 { int a; } __attribute__((deprecated)) z3;
struct S4 { int a; } __attribute__((deprecated));
struct { int a; } __attribute__((deprecated)) z5, z6;
void f(void) { struct { int a; } __attribute__((deprecated)) l; (void)l; }
struct T { struct { int a; } __attribute__((deprecated)) m; };
union __attribute__((deprecated)) U { int a; } u1;
enum __attribute__((deprecated)) E { A } e1;
struct __attribute__((deprecated)) S7 { int a; } *z7;
enum __attribute__((deprecated)) { A1 } e;
enum __attribute__((deprecated)) { B1, B2 } e2, e3;
struct __attribute__((deprecated)) { int a; } s1;
