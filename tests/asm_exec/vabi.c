/* Vectors across the System V ABI (see vabi.h): our functions are exported, take and return
 * vectors in %xmm, and call gcc's. */
#include "vabi.h"

static int fold(v4si v) { return v[0] + v[1] * 3 + v[2] * 5 + v[3] * 7; }
static int foldf(v4sf v) { return (int)(v[0] + v[1] * 2 + v[2] * 3 + v[3] * 4); }
static int foldd(v2df v) { return (int)(v[0] * 3 + v[1]); }
static int foldh(v8hi v) { return v[0] + v[1] * 3 + v[2] * 5 + v[3] * 7 + v[4] * 11 + v[5] * 13 + v[6] * 17 + v[7] * 19; }

v4si mine_vadd(v4si a, v4si b) { return a + b * 3; }
v4sf mine_vf(v4sf a, float s, v4sf b) { return a * s - b; }
v2df mine_vd(int k, v2df a) { return a + k; }
v8hi mine_vh(v8hi a, int k) { return a * (short)k; }
v2si mine_v2(v2si a, int k) { return a + k; }
struct vs mine_vs(int k, struct vs a) { struct vs r; r.v = a.v * k; return r; }
struct vsf mine_vsf(float f, struct vsf a) { struct vsf r; r.v = a.v + f; return r; }
int mine_many(v4si a, int i, v4sf b, v4si c, double d, v2df e, v4si f, v4si g, v4si h, long long n)
{
    return fold(a) + i + foldf(b) + fold(c) * 2 + (int)d + foldd(e) + fold(f) * 3 + fold(g) * 4 + fold(h) * 5 + (int)n;
}

int t_vadd(int a, int b, int c, int d)
{
    v4si x = {a, b, c, d}, y = {d, c, b, a};
    return fold(ext_vadd(x, y)) + fold(mine_vadd(y, x));
}

int t_vf(int a, int b, int c, int d)
{
    v4sf x = {a & 255, b & 255, c & 255, d & 255}, y = {d & 255, c & 255, b & 255, a & 255};
    return foldf(ext_vf(x, (float)(a & 255) / 2, y)) + foldf(mine_vf(y, (float)(b & 255) / 4, x));
}

int t_vd(int a, int b, int c, int d)
{
    v2df x = {a & 255, b & 255};
    return foldd(ext_vd(c & 255, x)) + foldd(mine_vd(d & 255, x));
}

int t_vh(int a, int b, int c, int d)
{
    v8hi x = {a, b, c, d, d, c, b, a};
    return foldh(ext_vh(x, d)) + foldh(mine_vh(x, c));
}

int t_v2(int a, int b, int c, int d)
{
    v2si x = {a, b}, y = ext_v2(x, c), z = mine_v2(x, d);
    return y[0] + y[1] * 5 + z[0] * 7 + z[1] * 11;
}

int t_vs(int a, int b, int c, int d)
{
    struct vs x = {{a, b, c, d}}, y = ext_vs(a - d, x), z = mine_vs(b + c, x);
    struct vsf f = {{a & 255, b & 255, c & 255, d & 255}}, g = ext_vsf((c & 255) * 0.5f, f), h = mine_vsf((d & 255) * 0.25f, f);
    return fold(y.v) + fold(z.v) * 3 + foldf(g.v) + foldf(h.v) * 2;
}

int t_many(int a, int b, int c, int d)
{
    v4si x = {a, b, c, d}, y = {d, a, b, c}, z = {c, d, a, b}, w = {b, c, d, a};
    v4sf f = {a & 255, b & 255, c & 255, d & 255};
    v2df e = {a & 255, b & 255};
    return ext_many(x, a, f, y, (b & 255) * 0.5, e, z, w, x, (long long)c * d) + mine_many(w, d, f, x, (a & 255) * 0.5, e, y, z, w, (long long)a * b);
}

int t_indirect(int a, int b, int c, int d)
{
    v4si (*fp)(v4si, v4si) = (a & 1) ? ext_vadd : mine_vadd;
    v4si x = {a, b, c, d}, y = {d, c, b, a};
    return fold(fp(x, y)) + fold(fp(y, fp(x, x)));
}

int t_local(int a, int b, int c, int d)    /* vectors computed between calls */
{
    v4si x = {a, b, c, d};
    x = ext_vadd(x, mine_vadd(x, x)) + x;
    x = mine_vadd(ext_vadd(x, x), mine_vadd(x, x));
    return fold(x);
}
