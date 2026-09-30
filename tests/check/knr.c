int f(a, b) int a; { return a; }
int g(a) int a; int c; { return a; }
int h(a, a) int a; { return a; }
int k(a) long a; { return a; }
int m(a) register int a; { return a; }
int n(a) static int a; { return a; }
int p(a, b) int a, b; { return a + b; }
void q(a) float a; { }
int r(int);
int r(a) char a; { return a; }
int s();
int s(a) int a; { return a; }
int t(a) struct S a; { return 0; }
int u(a) int a[]; { return 0; }
