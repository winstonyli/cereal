int leaf(int a, int b) { return a + b; }
#define DEFINE_FN(name) int name(void) { return leaf(5, 6); }
DEFINE_FN(made)
DEFINE_FN(other)
int user(void) { return made() + other(); }
