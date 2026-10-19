/* Globals and static objects for tests/asm_exec.py: .data, .rodata and .bss,
 * addresses inside initializers (relocations), tentative definitions, and
 * block-scope statics.  The functions run in the same order under gcc and
 * ours, so state they leave behind is compared too. */
int counter;                    /* tentative: zeros in .bss */
int seeded = 40;
static int hidden = 7;
const int table[8] = {3, 1, 4, 1, 5, 9, 2, 6};
long long big = -5000000000LL;
unsigned char bytes[5] = {250, 251, 252, 253, 254};
short shorts[3] = {-1, 2, -3};
double ratio = 2.5;
float third = 0.33333334f;
int dup;
int dup = 11;

struct pair { int a; long long b; char c; };
struct pair pairs[3] = {{1, 100, 'x'}, {2, -200, 'y'}, {3, 300, 'z'}};
struct pair single;

const char *names[3] = {"red", "green", "blue"};
const char text[] = "fish";
char buf[6] = "ab";
int *inner = &seeded;
const int *mid = &table[1] + 2;
const char *tail = "hello" + 2;

struct flags { int lo : 3; unsigned hi : 5; int whole; };
struct flags fl = {-2, 21, 99};

int bump(int k)
{
    counter += k;
    return counter;
}

int mixed(int k)
{
    return seeded * 3 + hidden - k + dup;
}

int look(int k)
{
    return table[k & 7] * 10 + table[(k >> 3) & 7];
}

int wide(int k)
{
    big += k;
    return (int)(big >> 20) + bytes[k & 3] + shorts[(k & 0xff) % 3];
}

double scale(int k)
{
    ratio = ratio * 2 + k;
    return ratio + third;
}

int field(int k)
{
    int i = k & 1;
    pairs[i].a += k;
    pairs[i + 1].b -= k;
    single.c = (char)k;
    single.a = pairs[0].a + pairs[1].a + pairs[2].a;
    return single.a + (int)pairs[2].b + pairs[i].c + single.c;
}

int strings(int k)
{
    const char *s = names[(k & 0xff) % 3];
    int n = 0;
    while (s[n])
        n++;
    return n * 100 + s[0] + text[k & 3] + buf[k & 1] + tail[k & 1];
}

int points(int k)
{
    *inner += k;
    return *inner + *mid + inner[0] - seeded;
}

int local_static(int k)
{
    static int calls;
    static int memo[4] = {10, 20, 30, 40};
    calls++;
    memo[k & 3] += calls;
    return memo[k & 3] + calls;
}

int other_static(int k)
{
    static int calls = 100;
    static const char *label = "tag";
    calls += k;
    return calls + label[k & 1];
}

int bitfields(int k)
{
    unsigned char *p = (unsigned char *)&fl;
    return p[0] + fl.whole * k;
}

int add1(int x)
{
    return x + 1;
}

int dbl(int x)
{
    return x * 2;
}

double mix(double a, int b)
{
    return a * b + 0.5;
}

int sum7(int a, int b, int c, int d, int e, int f, int g)
{
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g;
}

int (*ops[2])(int) = {add1, dbl};
int (*cur)(int) = dbl;
double (*fd)(double, int) = mix;
int (*many)(int, int, int, int, int, int, int) = sum7;

int dispatch(int k)
{
    int r = ops[k & 1](k) + cur(k);
    cur = ops[(k >> 1) & 1];
    return r + many(k, 1, 2, 3, 4, 5, 6) + (int)fd(1.5, k & 7);
}

int through(int (*f)(int), int k)
{
    return f(f(k)) + (*f)(1);
}

int usethrough(int k)
{
    return through(k & 1 ? add1 : dbl, k);
}

int nothing(void)
{
    return 5 + counter;
}

/* symbols this unit only declares: their addresses come through the GOT */
extern int puts(const char *);
extern char **environ;
int got(int k)
{
    int (*f)(const char *) = puts;
    return (f != 0) + (environ != 0) + (&environ != 0) + k;
}

/* an extern array of unknown size, and an old-style empty parameter list */
extern const char *const ext_names[];
int ext_at(int i) { return ext_names[i & 1][0] + i; }
int noproto() { return 11; }
int callnoproto(int k) { return k + noproto(); }

/* weak references: an undefined weak function is called only when present */
void weak_missing(void) __attribute__((weak));
int weak_present(int k) { return weak_missing ? k : -k; }
int weak_call(int k) { if (weak_missing) weak_missing(); return k * 2; }
int weak_ne(int k) { return ((void (*)(void))0 != weak_missing) * 7 + (weak_missing != 0) * 3 + (weak_missing == (void (*)(void))0) * 5 + k; }
/* the only reference to an undefined weak function is a call that is not made */
void weak_gone(int) __attribute__((weak));
int weak_only(int k) { if (k == 123456) weak_gone(k); return k + 1; }
