struct data { char c; int *ptr; } __attribute__((packed));
int *a(struct data *p) { return (int *) &p->ptr; }
int *b(struct data *p) { return p ? (int *) &p->ptr : (int *) 0; }
int *c(struct data *p, int *x) { return (*x = 1, (int *) &p->ptr); }
int *d(struct data *p, int *x) { return p ? (*x = 1, (int *) &p->ptr) : (int *) 0; }
int *e(struct data *p, int *x) { int *q; q = p ? (*x = 1, (int *) &p->ptr) : (int *) 0; return q; }
int *f(struct data *p, int *x) { return p ? (*x = 1, (int *) &(p->ptr)) : 0; }
int *g(struct data *p, int *x) { return (p ? (int *) &p->ptr : 0); }
int *h(struct data *p, int *x) { return (1, (int *) &p->ptr); }
