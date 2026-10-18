// T1 conversions: promotions, usual arithmetic conversions, shifts, comparisons.
enum E { A, B };
enum N { M = -1, P };
int cc(char a, char b) { return a + b; }
int sh(short a, long b) { return a << b; }
int sh2(char a, char b) { return a >> b; }
int ucmp(unsigned a, int b) { return a < b; }
int en(enum E a, enum E b) { return a + b; }
int ens(enum N a) { return a + 1; }
int uc(unsigned char a, unsigned char b) { return a * b; }
long ll(unsigned a, long b) { return a + b; }
unsigned ul(unsigned a, long b) { return a + b; }
int neg(unsigned char a) { return -a + ~a + +a; }
int lg(int a, int b) { return a && b || !a; }
int fold(int a) { return a + (1 + 2) * 3 + (char)300 + sizeof(int); }
long wide(int a) { return a; }
int narrow(long a) { return (int)a; }
void vd(int a) { (void)a; a, a; }
