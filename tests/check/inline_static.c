static int s;
static int f(void) { return 0; }
inline int g(void) { return s + f(); }
inline int h(void) { static int n; return n; }
