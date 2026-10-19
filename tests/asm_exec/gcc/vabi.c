/* The gcc-built side of tests/asm_exec/vabi.c: callees we call, and gcc_* callers of ours. */
#include "../vabi.h"

static int fold(v4si v) { return v[0] + v[1] * 3 + v[2] * 5 + v[3] * 7; }
static int foldf(v4sf v) { return (int)(v[0] + v[1] * 2 + v[2] * 3 + v[3] * 4); }
static int foldd(v2df v) { return (int)(v[0] * 3 + v[1]); }
static int foldh(v8hi v) { return v[0] + v[1] * 3 + v[2] * 5 + v[3] * 7 + v[4] * 11 + v[5] * 13 + v[6] * 17 + v[7] * 19; }

v4si ext_vadd(v4si a, v4si b) { return a + b * 3; }
v4sf ext_vf(v4sf a, float s, v4sf b) { return a * s - b; }
v2df ext_vd(int k, v2df a) { return a + k; }
v8hi ext_vh(v8hi a, int k) { return a * (short)k; }
v2si ext_v2(v2si a, int k) { return a + k; }
struct vs ext_vs(int k, struct vs a) { struct vs r; r.v = a.v * k; return r; }
struct vsf ext_vsf(float f, struct vsf a) { struct vsf r; r.v = a.v + f; return r; }
int ext_many(v4si a, int i, v4sf b, v4si c, double d, v2df e, v4si f, v4si g, v4si h, long long n)
{
    return fold(a) + i + foldf(b) + fold(c) * 2 + (int)d + foldd(e) + fold(f) * 3 + fold(g) * 4 + fold(h) * 5 + (int)n;
}

int gcc_vadd(int a, int b, int c, int d)
{
    v4si x = {a, b, c, d}, y = {d, c, b, a};
    return fold(mine_vadd(x, y));
}

int gcc_vf(int a, int b, int c, int d)
{
    v4sf x = {a & 255, b & 255, c & 255, d & 255}, y = {d & 255, c & 255, b & 255, a & 255};
    return foldf(mine_vf(x, (float)(a & 255) / 2, y));
}

int gcc_vd(int a, int b, int c, int d)
{
    v2df x = {a & 255, b & 255};
    return foldd(mine_vd(c & 255, x));
}

int gcc_vh(int a, int b, int c, int d)
{
    v8hi x = {a, b, c, d, d, c, b, a};
    return foldh(mine_vh(x, d));
}

int gcc_v2(int a, int b, int c, int d)
{
    v2si x = {a, b}, y = mine_v2(x, c);
    return y[0] + y[1] * 5;
}

int gcc_vs(int a, int b, int c, int d)
{
    struct vs x = {{a, b, c, d}}, y = mine_vs(a - d, x);
    struct vsf f = {{a & 255, b & 255, c & 255, d & 255}}, g = mine_vsf((c & 255) * 0.5f, f);
    return fold(y.v) + foldf(g.v);
}

int gcc_many(int a, int b, int c, int d)
{
    v4si x = {a, b, c, d}, y = {d, a, b, c}, z = {c, d, a, b}, w = {b, c, d, a};
    v4sf f = {a & 255, b & 255, c & 255, d & 255};
    v2df e = {a & 255, b & 255};
    return mine_many(x, a, f, y, (b & 255) * 0.5, e, z, w, x, (long long)c * d);
}
