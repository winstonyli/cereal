/* Expression control flow for --verify-cfg (docs/TAST_T8_PLAN.md, T8b). */
int g(int);
int h(int, int);
void v(int);

int conds(int a, int b, int c)
{
    int r = 0;
    if (a && b) r = 1;
    if (a || b && c) r += 2;
    if (!(a && b) || c) r += 4;
    while (a && b > 0) { b--; r++; }
    for (; a || c; c--) r++;
    do r--; while (r > 0 && c);
    return r;
}

int values(int a, int b, int c)
{
    int x = a && b;
    int y = a || b;
    int z = a ? b : c;
    int w = a ?: c;
    return x + y + z + w + (a ? b && c : b || c);
}

int pending(int a, int b)
{
    int r = g(a) + (a && b);
    r += g(r) * (b ? g(a) : g(b));
    r += h(g(a), b || a);
    return r + (a, b) + (g(a), b && a);
}

void voids(int a, int b)
{
    a ? v(1) : v(2);
    b && (a = 3);
    (a || b) ? v(a) : (void)0;
}

int stmts(int a, int b)
{
    int x = ({ int t = a; t + b; });
    int y = g(a) + ({ int u = g(b); if (u) u = 2; u; });
    x += a ? ({ b; }) : 0;
    return x + y;
}

int inits(int a, int b)
{
    int p[3] = { a, b && a, g(b) };
    struct { int x, y; } s = { g(a), a ? b : 1 };
    return p[1] + s.y;
}

int sw(int a, int b)
{
    switch (a && b) { case 0: return 1; default: return 2; }
}

int rets(int a, int b)
{
    return a ? g(a) : b ? g(b) : 0;
}
