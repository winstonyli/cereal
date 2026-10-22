/* Memory functions for tests/eval_sweep.sh: locals, arrays, pointers into them,
 * structs.  Every access stays inside its object; no pointer escapes. */

struct P {
    int a;
    short b;
    long c;
};

int arr(int i, int j)
{
    int a[8];
    int k;
    for (k = 0; k < 8; k++)
        a[k] = k * i;
    return a[j & 7] + a[(j + 1) & 7];
}

int ptr(int x)
{
    int y = x;
    int *p = &y;
    *p += 3;
    return *p + y;
}

long st(int x, short y)
{
    struct P s;
    struct P *q = &s;
    s.a = x;
    s.b = y;
    s.c = x * 2L;
    q->a += 1;
    return q->a + q->b + q->c;
}

int walk(int n)
{
    int a[6];
    int *p = a;
    int *e = a + 6;
    int s = 0;
    n &= 7;
    while (p < e)
        *p++ = n++;
    for (p = a; p != e; p++)
        s = s * 3 + *p;
    return s + (int)(e - a);
}

int cond(int x, int y)
{
    int a[2];
    int *p = x ? &a[0] : &a[1];
    a[0] = x;
    a[1] = y;
    *p = *p && y ? *p + 1 : 7;
    return a[0] * 5 + a[1];
}
