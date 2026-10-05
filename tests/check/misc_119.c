// flags: -pedantic
/* An initializer for the flexible member of a struct with no named members is
 * silent: gcc keeps the member with an erroneous type. */
struct t { char b[]; };
struct t t1 = { .b = "" };
struct t t2 = { "" };
struct t t3 = { };
int f(struct t *q) { return q->b[0] + sizeof(struct t) + sizeof *q; }
struct t t4;
