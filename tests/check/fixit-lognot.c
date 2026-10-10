// flags: -Wall -fdiagnostics-parseable-fixits
int f(int a, int b) { return !a == b; }
int g(int a, int b) { return !(a + 1) < b; }
int h(int a, int b) { return !a & b; }
int k(int b) { return !5 == b; }
