#define e0 "a"
#define e1 e0 e0 e0 e0 e0 e0 e0 e0 e0 e0
#define e2 e1 e1 e1 e1 e1 e1 e1 e1 e1 e1
#define e3 e2 e2 e2 e2 e2 e2 e2 e2 e2 e2
#define e4 e3 e3 e3 e3 e3 e3 e3 e3 e3 e3
#define e5 e4 e4 e4 e4 e4 e4 e4 e4 e4 e4
void foo(void) { (void)(e5); int x = (e5); }
