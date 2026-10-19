/* Our side of the struct ABI test (see abi.h): calls into gcc-built code and
 * callees that gcc-built callers call. */
#include "abi.h"

struct s2l mine_s2l(struct s2l a, int k)
{
    struct s2l r = {a.a + k, a.b * 2};
    return r;
}

struct sdi mine_sdi(int k, struct sdi a)
{
    struct sdi r = {a.d + k, a.l - k};
    return r;
}

struct sbig mine_sbig(struct sbig a, int k)
{
    struct sbig r = a;
    r.v[0] += k;
    r.v[4] = a.v[1] * 3;
    return r;
}

struct sid mine_sid(struct sid a, struct sff b)
{
    struct sid r = {a.i + (int)b.x, a.d + b.y};
    return r;
}

long long mine_take(struct s2i a, struct s2l b, struct sd c, struct sdd d, struct sid e,
                    struct sdi f, struct sff g, struct sf3 h, struct sch i, struct nest j,
                    union ul u)
{
    return a.a + a.b * 3 + b.a * 5 + b.b + (long long)(c.d * 4) + (long long)(d.x + d.y * 2) +
           e.i + (long long)(e.d * 3) + (long long)f.d + f.l + (long long)(g.x * 7 + g.y) +
           (long long)(h.v[0] + h.v[1] * 3 + h.v[2]) + i.a + i.b * 2 + j.p.a + j.p.b +
           (long long)(j.q.d * 8) + u.l;
}

int rets(int k)
{
    struct s2i a = ext_s2i(k);
    struct s2l b = ext_s2l(k);
    struct sd c = ext_sd(k);
    struct sdd d = ext_sdd(k);
    struct sid e = ext_sid(k);
    struct sdi f = ext_sdi(k);
    struct sff g = ext_sff(k);
    struct sf3 h = ext_sf3(k);
    struct sch i = ext_sch(k);
    struct sbig big = ext_sbig(k);
    struct nest n = ext_nest(k);
    union ul u = ext_ul(k);
    return (int)(a.a + a.b + b.a + b.b + (long long)(c.d + d.x + d.y + e.d + f.d + g.x + g.y) +
                 e.i + f.l + (long long)(h.v[0] + h.v[1] + h.v[2]) + i.a + i.b + big.v[0] +
                 big.v[3] + n.p.a + n.p.b + (long long)n.q.d + u.l);
}

int args(int k)
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
    return (int)ext_take(a, b, c, d, e, f, g, h, i, j, u) + (int)mine_take(a, b, c, d, e, f, g, h, i, j, u);
}

int stacky(int k)
{
    struct sbig big = ext_sbig(k);
    struct s2l l = {k, -k};
    struct sdd y = {0.5, k * 0.125};
    return (int)ext_big(1, 2, 3, 4, k, big, l, 9) + (int)ext_spill(1, 2, 3, 4, 5, l, 2.5, y) +
           (int)ext_spill(1, 2, 3, 4, k, l, 2.5, y);
}

int chain(int k)
{
    struct s2l a = ext_s2l(k);
    struct s2l b = mine_s2l(mine_s2l(a, k), 1);
    struct sdi c = mine_sdi(k, ext_sdi(k));
    return (int)(b.a + b.b + (long long)c.d + c.l);
}

int transparent(int k)
{
    int x = k;
    return (int)ext_tp(&x);
}
