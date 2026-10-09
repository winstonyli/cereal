/* B3 index data (docs/B3_DESIGN.md section 3): types, parents, readonly and
 * static, nameless records and enums, the innermost scope of block names */
typedef struct { int x; } Anon;
typedef Anon *AnonP;
typedef const int CI;
enum { RED, GREEN };
struct Out { union { int a; struct { int b; }; }; struct { int c; } named; };
typedef int (*cmp_fn)(const void *, const void *);
static const int table[3] = {1, 2, 3};
CI limit = 4;
const char *msg;
static Anon make(AnonP p);
cmp_fn cmp;
int run(int n, cmp_fn f)
{
    static int calls;
    for (int i = 0; i < n; i++) {
        int sq = i * i;
        calls += sq + table[i % 3] + limit + RED;
    }
    {
        enum { BLUE } shade = BLUE;
        return calls + shade + f(msg, msg);
    }
}
static Anon make(AnonP p) { return *p; }
