/* Statement control flow for --verify-cfg (docs/TAST_T8_PLAN.md, T8a). */
int g(int);

int ifs(int a, int b)
{
    int r = 0;
    if (a) r = 1;
    if (b) r += 2; else r += 3;
    if (a) { if (b) r = 4; else r = 5; } else r = 6;
    return r;
}

int loops(int n)
{
    int s = 0, i;
    while (n > 0) { s += n; n--; if (s > 100) break; if (n & 1) continue; s++; }
    do { s--; if (s < 0) continue; s += 2; } while (s < 10);
    for (i = 0; i < n; i++) { s += i; if (i == 3) continue; s++; }
    for (;;) { if (++s > 50) break; }
    for (i = 0;; i++) { if (i > 5) break; }
    for (int j = 0; j < 3; j++) s += j;
    return s;
}

int sw(int x)
{
    int r = 0;
    switch (x) {
    case 1: r = 1;
    case 2: r += 2; break;
    case 3: case 4: r = 4; break;
    default: r = 9;
    case 5: r++;
    }
    switch (x) { case 1: r = 1; }
    switch (x) { r = 7; }
    return r;
}

int jumps(int a)
{
    int r = 0;
    goto start;
back:
    r++;
    if (r > 3) goto done;
start:
    r += a;
    goto back;
done:
    return r;
}

int dead(int a)
{
    return a;
    a = 1;
    if (a) return 2;
}

void falls(int *p)
{
    *p = 1;
}

int hidden(int a, int b)
{
    return a && b ? g(a) : (a || b);
}

int nest(int n)
{
    int s = 0;
    for (int i = 0; i < n; i++) {
        switch (i & 3) {
        case 0: continue;
        case 1: s++; break;
        default:
            while (s < 100) { s += 10; if (s == 50) break; }
        }
        s++;
    }
    return s;
}
