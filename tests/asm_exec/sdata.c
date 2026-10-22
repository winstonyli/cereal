/* Static initializers the evaluator folds (E5) for tests/asm_exec.py: gcc's
 * saturating float-to-integer conversions, comparisons, division and ?: in
 * constants, pointers to later and earlier objects (a cycle), pointers to
 * strings and into them, function pointers, nested aggregates with addresses,
 * and designated and partial lists.  Functions read the objects, so the bytes
 * and relocations are compared through the values gcc's build gives. */
int sat_i = 1e30;
int sat_n = -1e30;
unsigned sat_u = -1.0;
unsigned sat_h = 1e30;
long long sat_l = -1e30;
unsigned long sat_ul = 1e30;
unsigned char sat_c = 300.5;
int fold[6] = { 7 / 2, 7 % 4, (3 > 2) + (2 > 3), 1 ? 5 : 6, -(-9 >> 1), ~0u >> 28 };
long cmp = (3 == 3) * 10 + (1 ? 5 : 6) + (2 != 2);
double dd = (float)0.1;
float ff = 1 / 3.0;

struct node { int v; struct node *next; const char *name; };
extern struct node n2;
struct node n1 = { 1, &n2, "one" };
struct node n2 = { 2, &n1, "one" + 1 };
struct node *roots[3] = { &n1, &n2, 0 };
const char *pool[] = { "ab", "ab", "cd" + 1, 0 };
char *mid = (char *)&n1 + 4;
int arr[10] = { [3] = 7, [8] = 9, 1 };
int *ap = &arr[3];
int *ae = arr + 10;
long diff = (char *)&arr[7] - (char *)&arr[2];

int f1(int k) { return k + 1; }
int f2(int k) { return k * 2; }
int (*ftab[3])(int) = { f1, f2, f1 };
struct vt { int (*op)(int); const char *tag; int arg; } vt[2] = { { f2, "dbl", 3 }, { f1, "inc", 4 } };

struct inner { short a; char b[3]; double d; };
struct outer { char c; struct inner in[2]; struct inner *p; } out = { 'q', { { 1, "xy", 0.5 }, { -2, { 'a' } } }, &out.in[1] };

int sd_sat(int k)
{
    return (int)((sat_i ^ sat_n) + (int)sat_u * 3 + (sat_u | sat_h) + (sat_l >> 60) + (long long)(sat_ul >> 60) + sat_c + k);
}

int sd_fold(int k)
{
    return fold[(unsigned)k % 6] * 100 + (int)cmp + (int)(dd * 1000) + (int)(ff * 1000) + k;
}

int sd_list(int k)
{
    struct node *n = roots[k & 1];
    return n->v * 1000 + n->next->v * 100 + n->name[0] + n->next->name[0] + (roots[2] == 0)
         + (int)(mid - (char *)&n1);
}

int sd_pool(int k)
{
    const char *s = pool[(unsigned)k % 3];
    return s[0] * 3 + s[1] + (pool[3] == 0);
}

int sd_arr(int k)
{
    return arr[(unsigned)k % 10] * 10 + *ap + (ae - ap) + (int)diff + (ap == &arr[3]);
}

int sd_call(int k)
{
    return ftab[(unsigned)k % 3](k) + vt[k & 1].op(vt[k & 1].arg) + vt[k & 1].tag[0];
}

int sd_nest(int k)
{
    return out.c + out.in[k & 1].a * 10 + out.in[0].b[1] + (int)(out.in[0].d * 4) + out.p->a + (out.p == &out.in[1]);
}
