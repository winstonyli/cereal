/* the six relations on signed, unsigned, floating and pointer operands, run and folded;
 * eval.sh compares the evaluator with gcc */
#include <stdio.h>

#define SIX(a, b) (((a) < (b)) + 2 * ((a) > (b)) + 4 * ((a) <= (b)) + 8 * ((a) >= (b)) + \
                   16 * ((a) == (b)) + 32 * ((a) != (b)))

static int si(long long a, long long b) { return SIX(a, b); }
static int ui(unsigned long long a, unsigned long long b) { return SIX(a, b); }
static int fl(double a, double b) { return SIX(a, b); }
static int sh(int a, unsigned b) { return SIX(a, b); }      /* -1 against 1 becomes unsigned */

static int arr[4];

int main(void)
{
    volatile double zero = 0.0;
    double nan = zero / zero;
    long long s[] = {-2, -1, 0, 1, 2, 0x7fffffffffffffffLL, -0x7fffffffffffffffLL - 1};
    unsigned long long u[] = {0, 1, 2, ~0ULL, 1ULL << 63};
    double d[] = {-1.5, 0.0, 1.5, 1e300, 0.0 / 1.0};
    int i, j, r = 0;
    for (i = 0; i < 7; i++)
        for (j = 0; j < 7; j++)
            r = r * 31 + si(s[i], s[j]);
    for (i = 0; i < 5; i++)
        for (j = 0; j < 5; j++)
            r = r * 31 + ui(u[i], u[j]) + fl(d[i], d[j]);
    for (i = 0; i < 5; i++)
        r = r * 31 + fl(nan, d[i]) + 64 * fl(d[i], nan) + 4096 * fl(nan, nan);
    printf("%d %d %d\n", sh(-1, 1), sh(1, 1), sh(0, 1));
    printf("%d %d %d %d\n", SIX(1, 2), SIX(2u, 1u), SIX(-1, 1u), SIX(1.5, 1.5));
    printf("%d %d %d\n", SIX(&arr[0], &arr[2]), SIX(&arr[3], &arr[3]), SIX(&arr[3], &arr[1]));
    printf("%d\n", r);
    return r & 127;
}
