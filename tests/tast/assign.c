// T1 assignment: plain, compound, ++/--, _Bool, a ?: b.
_Bool bl(_Bool a, int b) { _Bool c = b; c = a; return !c; }
int elv(int a, int b) { return a ?: b; }
int cnd(char a, long b, int c) { return c ? a : b; }
unsigned char inc(unsigned char x) { x++; ++x; x += 300; x <<= 1; return x; }
int mix(short s, unsigned u) { s += u; u -= s; return s * u; }
int decl(int a) { int x = a, y = 'a'; unsigned char z = 255; { int x = 2; y = x; } return x + y + z; }
