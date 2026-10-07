// flags: -Wall
foo
int x = a;
int y = b;
int a, b;
int f(void) { return (a++ - a--); }
int g(void) { return a++ - a--; }
int h(void) { b = (a++ - a--); return b; }
int i(void) { (a++ - a--); return b; }
int k(void) { return a++ + a++; }
int l(void) { return a = a++; }
int m(void) { return a++ ? a-- : 1; }
