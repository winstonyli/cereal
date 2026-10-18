/* TAST T2 layout regression: tests/tast_layout.py compares each get_<f>'s
 * field node (byte offset, bit offset, width) with where gcc puts <f>. */
#define FIELDS(X) X(a) X(b) X(c) X(d) X(e) X(f) X(g) X(h) X(i) X(j) X(k) X(l) \
    X(m) X(n) X(o) X(p) X(q) X(r)
struct S {
    char a;
    int b : 3;
    unsigned c : 5;
    unsigned d : 12;
    unsigned e : 20;
    long f : 40;
    char g;
    int : 0;
    short h;
    unsigned i : 1;
    union { int j; char k; };
    struct { char l; long m; };
    _Bool n : 1;
    unsigned long long o : 33;
    int p : 31;
    unsigned char q : 4;
    unsigned char r : 7;
};
#define GET(x) long get_##x(struct S *p) { return p->x; }
FIELDS(GET)

struct __attribute__((packed)) K {
    char a;
    int b;
    short c : 5;
    int d : 20;
    char e;
};
#define KFIELDS(X) X(a) X(b) X(c) X(d) X(e)
#define KGET(x) long kget_##x(struct K *p) { return p->x; }
KFIELDS(KGET)

struct __attribute__((aligned(16))) L { char a; long b; char c; };
#define LFIELDS(X) X(a) X(b) X(c)
#define LGET(x) long lget_##x(struct L *p) { return p->x; }
LFIELDS(LGET)

_Static_assert(sizeof(struct S) == 56, "S");
_Static_assert(sizeof(struct K) == 10, "K");
_Static_assert(sizeof(struct L) == 32, "L");
