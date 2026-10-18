/* T3c: statement expressions and __func__. */
extern void fail(const char *, const char *, unsigned, const char *);
int g(int);

#define ASSERT(c) ((void)sizeof((c) ? 1 : 0), __extension__ ({ if (c) ; else fail(#c, __FILE__, __LINE__, __extension__ __PRETTY_FUNCTION__); }))

int val(int a) { return ({ int t = a * 2; t + 1; }); }
void eff(int a) { ({ g(a); g(a + 1); }); }
int nested(int a) { return ({ int x = ({ a + 1; }); x * x; }); }
void asrt(int a) { ASSERT(a > 0); }
const char *fn(void) { return __func__; }
const char *pf(void) { return __PRETTY_FUNCTION__; }
int jump(int a) { return ({ if (a) return 1; 2; }); }
long wide(int a) { return ({ a; }); }
void empty(void) { ({ }); }
int lbl(int a) { return ({ int r = 0; again: r++; if (r < a) goto again; r; }); }
