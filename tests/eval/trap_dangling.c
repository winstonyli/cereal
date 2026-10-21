// trap: after its lifetime
static int *f(void) { int x = 3; return &x; }
int main(void) { return *f(); }
