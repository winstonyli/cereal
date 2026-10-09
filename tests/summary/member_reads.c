/* Member lookup in a record past the name-index threshold (16 members):
 * a unit reads the anonymous members a search in place reads, those before
 * the one holding the name (all of them for a miss), none for a member
 * named directly; by access and by designator alike. */
struct Big {
    int m0, m1, m2, m3, m4, m5, m6, m7, m8, m9;
    int m10, m11, m12, m13, m14, m15, m16, m17, m18, m19;
    struct { int a1; struct { int a2; }; };
    union { int u1; };
    int last;
};
int direct(struct Big *p) { return p->m3 + p->last; }
int first(struct Big *p) { return p->a2; }
int second(struct Big *p) { return p->u1; }
int miss(struct Big *p) { return p->none; }
struct Big d1 = { .a2 = 1 };
struct Big d2 = { .u1 = 1 };
struct Big d3 = { .m19 = 1 };
