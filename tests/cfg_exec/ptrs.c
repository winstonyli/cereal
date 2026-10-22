/* Pointer and record parameters for tests/eval_sweep.sh: the sweep hands each pointer a
 * fresh object of random bytes and compares the result and the final bytes of every object.
 * Every access stays inside the 16 elements (64 bytes for char and void) it is given. */

struct V {
    int x;
    short y;
    char z;
    long w;
    int arr[3];
    double d;
};

union U {
    int i;
    float f;
};

void fill(int *p, int v)
{
    int i;
    for (i = 0; i < 16; i++)
        p[i] = v + i;
}

int sum(const int *p)
{
    int i, s = 0;
    for (i = 0; i < 16; i++)
        s += p[i];
    return s;
}

void swap2(int *a, int *b)
{
    int t = a[0];
    a[0] = b[0];
    b[0] = t;
}

unsigned char *firstbig(unsigned char *p, int lim)
{
    int i;
    for (i = 0; i < 64; i++)
        if (p[i] > lim)
            return p + i;
    return 0;
}

char *cat(char *dst, const char *src)
{
    int i = 0;
    while (i < 31 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    return dst + i;
}

long vsum(struct V v)
{
    return v.x + v.y + v.z + v.w + v.arr[0] + v.arr[2] + (long)v.d;
}

void vbump(struct V *v, int n)
{
    v->x += n;
    v->arr[1] = v->y * n;
    v->d = v->d * 2 + n;
    if (v->z > 3)
        v->z = 0;
}

int uadd(union U u, int k)
{
    return u.i + k;
}

void zero(void *p, int n)
{
    unsigned char *q = p;
    int i;
    for (i = 0; i < 64 && i < n; i++)
        q[i] = 0;
}

int *pick(int *a, int *b, int sel)
{
    return sel > 0 ? a + (sel & 7) : b;
}

double dsum(const double *p, float scale)
{
    double s = 0;
    int i;
    for (i = 0; i < 16; i++)
        s += p[i] * scale;
    return s;
}

void cnt(int **pp, int k)
{
    if (pp[0])
        *pp[0] = k;
}

struct Node {
    int val;
    struct Node *next;
};

int chain(struct Node *n)
{
    int s = 0, i;
    for (i = 0; i < 3 && n; i++, n = n->next)
        s += n->val;
    return s;
}

void setnext(struct Node *n, int v)
{
    if (n->next)
        n->next->val = v + n->val;
}

int *inner(int **pp, int k)
{
    return pp[k & 3];
}

struct Buf {
    char *data;
    int len;
};

int blen(struct Buf b)
{
    int i, s = 0;
    for (i = 0; i < b.len && i < 16; i++)
        s += b.data[i];
    return s;
}

void bset(struct Buf *b, int c)
{
    int i;
    for (i = 0; i < 16; i++)
        b->data[i] = (char)(c + i);
}

long span(const int *a, const int *b)
{
    return b - a;
}

int count(const char *s, const char *end, char c)
{
    int n = 0;
    while (s < end) {
        if (*s++ == c)
            n++;
    }
    return n;
}

void rev(int *lo, int *hi)
{
    while (lo < hi) {
        int t = *lo;
        *lo++ = *--hi;
        *hi = t;
    }
}
