/* A flexible array member is a field of incomplete array type that only decays. */
struct C { int n; unsigned char d[]; };
void *b(struct C *c) { return c->d; }
unsigned char *c2(struct C *c, int i) { return c->d + i; }
int c3(struct C *c, int i) { return c->d[i]; }
