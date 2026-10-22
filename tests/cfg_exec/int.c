/* Integer-only functions for tests/eval_sweep.sh: the lowered graph is run by the
 * evaluator and compared with gcc.  Every function is total (no division by
 * a possible zero, no shift by an out-of-range count, no overflow of signed
 * int: gcc runs with -fwrapv, the interpreter wraps). */

int tally;                              /* a global: reached only by calls */

int bump(int x)
{
    int y = x * 3 + 1;
    return y ^ (x >> 1);
}

int conds(int a, int b, int c)
{
    int r = 0;
    if (a && b)
        r = 1;
    if ((a && b) || c)
        r += 2;
    if (!(a || b) && c)
        r += 4;
    if (a ? b : c)
        r += 8;
    return r;
}

int values(int a, int b, int c)
{
    int x = a && b;
    int y = a || c;
    int z = a ? b + 1 : c - 1;
    int w = a ?: b;
    int v = (a, b, c);
    return x + 2 * y + 4 * z + 8 * w + v;
}

int pending(int a, int b)
{
    int r = a + (b ? a : b) * 3;
    int s = bump(a) + (a && b ? bump(b) : bump(a + b));
    int t = (a ? b : a) - (b ? a : b);
    return r ^ s ^ t;
}

int loops(int n)
{
    int s = 0, i;
    n &= 15;
    for (i = 0; i < n; i++) {
        if (i == 3)
            continue;
        if (i > 11)
            break;
        s += i;
    }
    while (n > 0) {
        s += n--;
        if (s > 100)
            break;
    }
    do {
        s ^= n++;
    } while (n < 5);
    return s;
}

int sw(int k, int d)
{
    int r = 0;
    switch (k & 7) {
    case 0:
        r = 10;
    case 1:
        r += 1;
        break;
    case 2:
    case 3:
        r = d;
        break;
    default:
        r = -1;
    case 6:
        r += 100;
    }
    return r;
}

int duff(int n)
{
    int c = 0;
    n &= 15;
    switch (n & 3) {
    case 0:
        while (n-- > 0) {
            c += 1;
    case 3:
            c += 2;
    case 2:
            c += 3;
    case 1:
            c += 4;
        }
    }
    return c;
}

int gotos(int a, int b)
{
    int r = 0;
    if (a)
        goto one;
    r = 1;
    goto done;
one:
    r = b ? 2 : 3;
    if (b)
        goto done;
    r += 10;
done:
    return r;
}

int stmts(int a, int b)
{
    int x = ({ int t = a + 1; t * (b ? t : 2); });
    int y = ({ a > b ? a : b; }) + ({ int u = b; u && a; });
    return x - y;
}

int inits(int a, int b)
{
    int p = a && b, q = p ? a : b, r = (q, a || b);
    return p + q + r;
}

unsigned short narrow(unsigned char a, short b)
{
    unsigned short r = a && b;
    r += (unsigned char)(a ? b : a);
    return (unsigned short)(r + (a ?: b));
}

long long wide(long long a, int b)
{
    long long r = a && b ? a * 3 : a - b;
    r ^= b || a;
    return r;
}

int recurse(int n)
{
    n &= 31;
    if (n <= 1)
        return n;
    return n & 1 ? recurse(n - 1) + 1 : recurse(n >> 1) + recurse(n >> 2);
}

int compound(int a, int b)
{
    int x = a;
    x += b ? a : 1;
    x <<= a ? 1 : 2;
    x -= a && b;
    return x;
}

int incs(int a, int b)
{
    int i = a, j = b, r;
    r = i++ + (b ? j++ : i--);
    r += ++i - j--;
    return r + i * 7 + j;
}

int unused(int a, int b, int c)
{
    return b ? c : 5;
}

int seven(int a, int b, int c, int d, int e, int f, int g)
{
    return a - b * 2 + c * 3 - d + e * 5 - f + g * 7;
}

int eight(int a, int b, int c, int d, int e, int f, long g, unsigned char h)
{
    return a - b * 2 + c * 3 - d + e * 5 - f + (int)(g * 7) - h * 11;
}

int callmany(int x, int y)
{
    return eight(x, y, x + y, x - y, 1, 2, 3, y) + seven(y, x, 1, 2, 3, 4, x * y) +
           eight(y, x, 1, 2, 3, 4, 5, 6);
}
