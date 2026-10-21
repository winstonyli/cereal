/* pointers, arrays, structs by value, string literals, function pointers */
struct P { int x, y; char tag[4]; };
union U { int i; unsigned char b[4]; };
static struct P mk(int x, int y) { struct P p = { x, y, { 'a', 'b', 'c', 0 } }; return p; }
static int sum(struct P p) { p.x += 1; return p.x + p.y + p.tag[1]; }
static int slen(const char *s) { int n = 0; while (*s++) n++; return n; }
static int add(int a, int b) { return a + b; }
static int mul(int a, int b) { return a * b; }
static void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }
int main(void)
{
    int a[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    int i, s = 0, x = 3, y = 4;
    struct P p = mk(5, 6), q;
    int (*f[2])(int, int) = { add, mul };
    union U u;
    char buf[6] = "hello";
    int *e = a + 8;
    q = p;
    q.y = 100;
    swap(&x, &y);
    for (i = 0; i < 8; i++)
        a[i] *= i;
    for (i = 0; &a[i] != e; i++)
        s += a[i];
    u.i = 0x01020304;
    s += u.b[0] + u.b[3];
    s += sum(p) + p.x + q.y + slen("twelve chars") + slen(buf);
    s += f[0](x, y) + f[1](x, y) + (int)(e - a) + x * 10 + y;
    s += "abc"[2] + buf[1];
    return s;
}
