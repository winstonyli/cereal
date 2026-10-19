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
