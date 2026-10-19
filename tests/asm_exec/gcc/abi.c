/* The gcc-built side of tests/asm_exec/abi.c: callees we call, and gcc_* callers
 * of ours (driven like the other functions). */
#include "../abi.h"

struct s2i ext_s2i(int k) { struct s2i r = {k, k * 3}; return r; }
struct s2l ext_s2l(int k) { struct s2l r = {k * 5LL, -k * 7LL}; return r; }
struct sd ext_sd(int k) { struct sd r = {k * 0.5}; return r; }
struct sdd ext_sdd(int k) { struct sdd r = {k * 1.5, k - 0.25}; return r; }
struct sid ext_sid(int k) { struct sid r = {k, k * 2.5}; return r; }
struct sdi ext_sdi(int k) { struct sdi r = {k + 0.5, k * 11LL}; return r; }
struct sff ext_sff(int k) { struct sff r = {(float)k, k * 0.5f}; return r; }
struct sf3 ext_sf3(int k) { struct sf3 r = {{(float)k, 2.0f, k * 0.25f}}; return r; }
struct sch ext_sch(int k) { struct sch r = {(char)k, (short)(k * 9)}; return r; }
struct sbig ext_sbig(int k)
{
    struct sbig r;
    int i;
    for (i = 0; i < 5; i++)
        r.v[i] = k * (i + 1LL);
    return r;
}
struct nest ext_nest(int k) { struct nest r = {{k, 4}, {k * 0.125}}; return r; }
union ul ext_ul(int k) { union ul r; r.l = k * 1000003LL; return r; }

long long ext_take(struct s2i a, struct s2l b, struct sd c, struct sdd d, struct sid e,
                   struct sdi f, struct sff g, struct sf3 h, struct sch i, struct nest j,
                   union ul u)
{
    return a.a + a.b * 3 + b.a * 5 + b.b + (long long)(c.d * 4) + (long long)(d.x + d.y * 2) +
           e.i + (long long)(e.d * 3) + (long long)f.d + f.l + (long long)(g.x * 7 + g.y) +
           (long long)(h.v[0] + h.v[1] * 3 + h.v[2]) + i.a + i.b * 2 + j.p.a + j.p.b +
           (long long)(j.q.d * 8) + u.l;
}

long long ext_big(int a, int b, int c, int d, int e, struct sbig big, struct s2l l, int z)
{
    return a + b * 2 + c * 3 + d * 4 + e * 5 + big.v[0] + big.v[2] * 7 + big.v[4] + l.a + l.b * 3 + z;
}

long long ext_spill(int a, int b, int c, int d, int e, struct s2l l, double x, struct sdd y)
{
    return a + b + c + d + e + l.a + l.b * 2 + (long long)(x * 4) + (long long)(y.x + y.y * 8);
}

int gcc_s2l(int k)
{
    struct s2l a = {k, k * 2LL}, r = mine_s2l(a, k);
    return (int)(r.a + r.b);
}

int gcc_sdi(int k)
{
    struct sdi a = {k * 0.5, k * 4LL}, r = mine_sdi(k, a);
    return (int)((long long)(r.d * 2) + r.l);
}

int gcc_sbig(int k)
{
    struct sbig a, r;
    int i;
    for (i = 0; i < 5; i++)
        a.v[i] = k + i;
    r = mine_sbig(a, k);
    return (int)(r.v[0] + r.v[1] * 2 + r.v[4] * 3);
}

int gcc_sid(int k)
{
    struct sid a = {k, k * 0.75};
    struct sff b = {(float)k, 2.0f};
    struct sid r = mine_sid(a, b);
    return (int)(r.i + (long long)(r.d * 4));
}

int gcc_take(int k)
{
    struct s2i a = {k, 2};
    struct s2l b = {k * 3LL, 4};
    struct sd c = {k * 0.5};
    struct sdd d = {1.5, k};
    struct sid e = {k, 2.5};
    struct sdi f = {k * 0.25, 9};
    struct sff g = {2.0f, (float)k};
    struct sf3 h = {{1.0f, (float)k, 3.0f}};
    struct sch i = {(char)k, 5};
    struct nest j = {{k, 6}, {0.5}};
    union ul u;
    u.l = k * 77LL;
    return (int)mine_take(a, b, c, d, e, f, g, h, i, j, u);
}

long long ext_tp(union tp p) { return *p.ip * 3LL; }
