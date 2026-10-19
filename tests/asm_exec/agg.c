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

/* compound literals: objects made at the expression, scalar and aggregate */
static int sum_pt(const struct pt *p)
{
    return p->x * 3 + p->y;
}

int complit(int k)
{
    struct pt copy = (struct pt){k, k + 1};
    const struct pt *p = &(const struct pt){k * 2, 5};
    int *q = &(int){k + 7};
    int *arr = (int[]){k, 2, 3, k * 4};
    return sum_pt(p) + sum_pt(&(struct pt){4, k}) + copy.x + *q + arr[0] + arr[3];
}

/* a member of a call's result, and of a member of it */
typedef struct { int a; double d; char c; } RF;
typedef struct { RF in; long l; } RO;
static RF mkrf(int k) { RF r = {k, k * 0.5, (char)(k + 1)}; return r; }
static RO mkro(int k) { RO r = {mkrf(k), k * 3L}; return r; }
double rfield(int k)
{
    return mkrf(k).a + mkrf(k + 1).d + mkrf(k).c + mkro(k).in.d + mkro(k - 1).l + mkro(k).in.a;
}

/* a conditional of aggregate values, and an aggregate assigned from one */
static RF pickrf(int k, int c) { return c ? mkrf(k) : mkrf(-k); }
double condrf(int k)
{
    RF r = (k & 1) ? mkrf(k) : mkrf(k + 5);
    RF s = pickrf(k, k & 2);
    r = (k & 4) ? s : r;
    return r.a + r.d + s.c + ((k & 8) ? mkrf(k) : mkrf(7)).a;
}

/* a memory-class aggregate parameter is copied from the stack while later register
 * parameters (%rdx, %rcx among them) are still unsaved */
struct tri { long a, b, c; };
static long takes_tri(long x, struct tri t, long y, long z, long w, long v, long u)
{
    return x + t.a * 2 + t.b * 3 + t.c * 5 + y * 7 + z * 11 + w * 13 + v * 17 + u * 19;
}
long memparam(int k)
{
    struct tri t = {k, k + 1, k * 3};
    return takes_tri(k, t, k + 2, k + 3, k + 4, k + 5, k + 6);
}

/* offsetof with an index that is not constant */
struct oa { int x; struct { char c[4]; long l; } b[3]; short s[5][2]; };
long offv(int k)
{
    return __builtin_offsetof(struct oa, b[k % 3].l) + __builtin_offsetof(struct oa, s[k & 3][k & 1]) +
           __builtin_offsetof(struct oa, b[1].c[k & 3]);
}

/* bit-fields: loads, stores, compound assignment, increments, initializers, signedness, widths */
struct bf { unsigned a : 3; int b : 5; unsigned c : 20; long d : 40; char e; unsigned f : 1; int g : 7; _Bool h : 1; };
struct bf2 { unsigned char x : 4; unsigned y : 12; unsigned long z : 64 - 16 - 2; unsigned w : 2; };
struct bf3 { char p; unsigned q : 20; short r : 3; };
long bitf(int k)
{
    struct bf s = {1, 2, 3, 4, 5, 1, -3, 1};
    struct bf2 t = {k & 15, k * 37, 0x123456789abUL * k, k & 3};
    struct bf3 u = {7, k * 1111, -2};
    long r = 0;
    s.b ^= k;
    s.a++;
    s.c += k * 100;
    s.d = k * 100000L - 9999999999L;
    s.g = -k;
    s.h = k > 3;
    r += s.a * 100 + s.b + s.c + (int)s.d + s.e + s.f * 7 + s.g + s.h;
    r += (long)s.d * 3;
    r += t.x + t.y + (long)(t.z % 1000003) + t.w;
    t.z = ~t.z + k;
    r += (long)(t.z % 999983) + (t.w = k);
    r += u.p + u.q + u.r;
    r += (u.r = k * 3);
    r += (u.q += 5);
    r += u.q--;
    r += u.q;
    r += --s.a + s.b++ + (s.f = k) + (s.c = -k);
    r += (s.b << 1) + (s.a * 2) + sizeof(s) + sizeof(t) + sizeof(u);
    return r;
}

/* a compound assignment, ++ and -- on a bit-field named through a call or an index */
static struct bf *pick(struct bf *a, int k) { return a + (k & 1); }
long bitse(int k)
{
    struct bf s[2] = {{1, 2, 3, 4, 5, 1, -3, 1}, {6, -5, 70000, -9, 8, 0, 20, 0}};
    long r = 0;
    pick(s, k)->b += k;
    pick(s, k + 1)->c *= 3;
    pick(s, k)->a++;
    r += pick(s, k)->g-- + --pick(s, k + 1)->g;
    r += (pick(s, k)->d -= k * 1000000L);
    r += s[k & 1].b + s[(k + 1) & 1].c + s[0].a + s[1].a + s[0].g + s[1].g + (int)s[0].d + (int)s[1].d;
    return r;
}
