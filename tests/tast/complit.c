/* T7: a compound literal is one lvalue op that holds its initializer. */
struct N { int u; }; struct T { long a; struct N n; int v[2]; };
int g(void); int h(void); void use(struct T *, int);
struct T assign(struct T *p, int i) { p[i++] = (struct T){ .a = 1, .n = (struct N){ .u = 2 } }; return p[i]; }
void addr(void) { use(&(struct T){ .a = 3 }, 4); }
int *decay(void) { return (int[]){ 1, 2, 3 }; }
long field(void) { return (struct T){ .a = 5 }.a; }
int scalar(void) { return (int){ 5 } + 1; }
char *str(void) { return (char[]){ "ab" }; }
void order(void) { use(&(struct T){ .a = g() }, h()); }
int *sp = (int[]){ 1, 2 };
struct T *tp = &(struct T){ .a = 7, .v = { 8, 9 } };
