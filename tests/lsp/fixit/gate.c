#define OR(x) x || 0
int f(int a, int b) { return OR(a && b); }
