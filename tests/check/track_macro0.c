// flags: -ftrack-macro-expansion=0 -Wall
#define BAD(x) (x + "s")
#define NEG(n) typedef char t##n[-1]
int f(int i) { return BAD(i); }
NEG(1);
