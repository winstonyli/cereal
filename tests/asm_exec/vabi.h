/* Vectors across the System V ABI for tests/asm_exec.py: vabi.c is compiled by us, gcc/vabi.c by
 * gcc; each side calls the other.  A 16-byte vector travels in one %xmm, an 8-byte one in the low
 * half of an %xmm. */
typedef int v4si __attribute__((vector_size(16)));
typedef float v4sf __attribute__((vector_size(16)));
typedef double v2df __attribute__((vector_size(16)));
typedef short v8hi __attribute__((vector_size(16)));
typedef int v2si __attribute__((vector_size(8)));
struct vs { v4si v; };
struct vsf { v4sf v; };

/* gcc-built callees (gcc/vabi.c) */
v4si ext_vadd(v4si a, v4si b);
v4sf ext_vf(v4sf a, float s, v4sf b);
v2df ext_vd(int k, v2df a);
v8hi ext_vh(v8hi a, int k);
v2si ext_v2(v2si a, int k);
struct vs ext_vs(int k, struct vs a);
struct vsf ext_vsf(float f, struct vsf a);
int ext_many(v4si a, int i, v4sf b, v4si c, double d, v2df e, v4si f, v4si g, v4si h, long long n);

/* our callees (vabi.c) */
v4si mine_vadd(v4si a, v4si b);
v4sf mine_vf(v4sf a, float s, v4sf b);
v2df mine_vd(int k, v2df a);
v8hi mine_vh(v8hi a, int k);
v2si mine_v2(v2si a, int k);
struct vs mine_vs(int k, struct vs a);
struct vsf mine_vsf(float f, struct vsf a);
int mine_many(v4si a, int i, v4sf b, v4si c, double d, v2df e, v4si f, v4si g, v4si h, long long n);
