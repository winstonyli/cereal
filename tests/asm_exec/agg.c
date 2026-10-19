/* Aggregate copies and initializer lists of automatics for tests/asm_exec.py.
 * Only integers cross the call boundary; structs, unions and arrays are
 * built, copied and read inside. */
struct pt { int x, y; };
struct mix { char c; long long l; short s; };
struct big { int v[40]; struct pt p; };
union u8 { long long l; unsigned char b[8]; double d; };
struct tag { char name[6]; unsigned char n; };
struct nest { struct pt a, b; int tail[3]; };

int copy_small(int k)
{
    struct pt a = {k, k * 3}, b;
    b = a;
    a.x = 99;
    return a.x + b.x * 10 + b.y;
}

int copy_mixed(int k)
{
    struct mix m = {(char)k, (long long)k << 33, (short)(k * 7)}, n;
    n = m;
    m.c = 0;
    return n.c + (int)(n.l >> 31) + n.s + m.c;
}

int copy_big(int k)
{
    struct big a, b;
    int i, sum = 0;
    for (i = 0; i < 40; i++)
        a.v[i] = i * k;
    a.p.x = k;
    a.p.y = -k;
    b = a;
    a.v[5] = 1000;
    for (i = 0; i < 40; i++)
        sum += b.v[i] * (i & 3);
    return sum + b.p.x + b.p.y + a.v[5];
}

int through_ptr(int k)
{
    struct pt s[3] = {{1, 2}, {3, 4}, {5, 6}};
    struct pt *p = &s[k & 1], *q = &s[2];
    *q = *p;
    p->x += 100;
    s[0] = s[2];
    return s[0].x + s[1].y * 10 + s[2].x * 100 + p->x;
}

int chain(int k)
{
    struct pt a = {k, 2}, b, c;
    c = b = a;
    b.x += 1;
    return a.x + b.x * 10 + c.x * 100 + c.y;
}

int unions(int k)
{
    union u8 u, v;
    u.l = (long long)k * 0x0101010101LL;
    v = u;
    u.b[0] = 7;
    return v.b[0] + v.b[4] + (int)(v.l >> 8) + u.b[0];
}

int partial(int k)
{
    int a[6] = {k, 2};
    struct pt p = {k};
    struct mix m = {1};
    int i, s = 0;
    for (i = 0; i < 6; i++)
        s += a[i] * (i + 1);
    return s + p.x * 3 + p.y + m.c + (int)m.l + m.s;
}

int zeros(int k)
{
    int a[40] = {0};
    char big[200] = {0};
    int i, s = k;
    a[k & 31] = 5;
    big[k & 127] = 1;
    for (i = 0; i < 40; i++)
        s += a[i] * i;
    for (i = 0; i < 200; i++)
        s += big[i];
    return s;
}

int strings(int k)
{
    char a[8] = "abc";
    char b[3] = "xyz";
    struct tag t = {"hi", 9};
    return a[k & 3] + a[4] + b[k % 3] + t.name[k & 1] + t.name[3] + t.n;
}

int nested(int k)
{
    struct nest n = {{k, 1}, {2, k}, {7, k}};
    struct nest m = {.b = {k}, .tail = {1}};
    struct pt q = n.b;
    struct nest o = n;
    o.a.x = 0;
    return n.a.x + n.b.y + n.tail[1] + m.b.x + m.tail[0] + m.a.y + q.x + o.tail[1] + o.a.x;
}

int exprs(int k)
{
    int j = k + 1;
    struct pt p = {j * 2, j - 3};
    struct pt arr[2] = {{p.y, p.x}, {k, k}};
    return arr[0].x + arr[0].y * 10 + arr[1].x;
}

/* Leaves stale bytes where the next call's frame will be, so a missed zero
 * fill reads them back. */
int dirty(int k)
{
    unsigned char junk[512];
    int i, s = 0;
    for (i = 0; i < 512; i++)
        junk[i] = (unsigned char)(0x5a + k);
    for (i = 0; i < 512; i += 64)
        s += junk[i];
    return s;
}

int zeros_after_dirt(int k)
{
    dirty(k);
    return zeros(k) + partial(k);
}
