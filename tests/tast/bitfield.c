/* TAST T2: bit-fields: load, store, truncation, promotion. */
struct B {
    int a : 3;
    unsigned b : 5;
    unsigned c : 1;
    _Bool d : 1;
    unsigned e : 32;
    long f : 40;
    int : 0;
    char g;
    unsigned h : 7;
};
int lda(struct B *p) { return p->a; }
unsigned ldb(struct B *p) { return p->b; }
int sum(struct B *p) { return p->a + p->b + p->c; }
long ldf(struct B *p) { return p->f; }
unsigned lde(struct B *p) { return p->e + 1; }
void st(struct B *p, int v) { p->a = v; p->b = v; p->d = v; }
int stv(struct B *p, int v) { return p->a = v; }
void inc(struct B *p) { p->a++; ++p->b; p->c += 1; p->h -= 2; }
int cmp(struct B *p) { return p->a < p->b; }
int viaobj(void) { struct B x; x.h = 100; x.g = 1; return x.h + x.g; }
