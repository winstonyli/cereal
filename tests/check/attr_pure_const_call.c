// flags: -Wall
/* Call equality (the self-comparison warning) needs the callee's pure/const fact: it must survive
 * a redeclaration without the attribute, a definition, copy(), and the pure/const exclusion.
 * gcc warns for const callees only (a pure call is not a tautology), so the pure ones stay silent. */
/* (second comment line keeps the line numbers of the goldens below) */
int c1(int) __attribute__((const));
int c2(int) __attribute__((const));
int c2(int);
int c3(int) __attribute__((const));
int c3(int a) { return a; }
int c4(int) __attribute__((copy(c1)));
int c5(int) __attribute__((const, pure));      /* pure ignored, const kept */
int p1(int) __attribute__((pure));
int p2(int) __attribute__((pure));
int p2(int);
int p3(int) __attribute__((pure));
int p3(int a) { return a; }
int p4(int) __attribute__((copy(p1)));
int p5(int) __attribute__((pure, const));      /* const ignored, pure kept */
int np(int);
// cereal omits gcc's "attribute ignored" here (PARKED.md); the call below stays silent as in gcc
int (*fp)(int) __attribute__((pure));
int t(int x)
{
    int r = 0;
    r += c1(x) == c1(x);
    r += c2(x) == c2(x);
    r += c3(x) == c3(x);
    r += c4(x) == c4(x);
    r += c5(x) == c5(x);
    r += p1(x) == p1(x);   /* pure: silent, as in gcc */
    r += p2(x) == p2(x);   /* pure: silent, as in gcc */
    r += p3(x) == p3(x);   /* pure: silent, as in gcc */
    r += p4(x) == p4(x);   /* pure: silent, as in gcc */
    r += p5(x) == p5(x);   /* pure: silent, as in gcc */
    r += np(x) == np(x);
    r += fp(x) == fp(x);
    return r;
}
