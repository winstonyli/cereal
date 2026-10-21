/* float and double: arithmetic in the type's precision, conversions, NaN compares, rounding */
static int cmps(double a, double b)
{
    return (a == b) | (a != b) << 1 | (a < b) << 2 | (a > b) << 3 | (a <= b) << 4 | (a >= b) << 5;
}

static float fadd(float a, float b) { return a + b; }
static double dmul(double a, double b) { return a * b; }

static int truth(double x)
{
    int r = 0;
    if (x)
        r |= 1;
    if (!x)
        r |= 2;
    while (x) {
        r |= 4;
        break;
    }
    return r | (x ? 8 : 0) | (x && 1) << 4 | (x || 0) << 5 | (!x) << 6;
}

int main(void)
{
    volatile double zero = 0.0, big = 1e300;
    double nan = zero / zero, inf = big * big;
    float f = 16777216.0f;
    double d = 0.1;
    int r = 0, i;
    unsigned long long u = 18446744073709551615ull;
    long long s = -9007199254740993ll;
    r += cmps(1.5, 2.5) + 64 * cmps(nan, nan) + 4096 * cmps(inf, -inf);
    r += (int)(f + 1.0f) == 16777216;            /* float rounding: 2^24 + 1 is not representable */
    r += 2 * ((double)(f + 1.0f) == 16777216.0);
    r += 4 * (fadd(0.1f, 0.2f) == 0.1f + 0.2f);
    r += 8 * (dmul(d, 3.0) != 0.3);
    r += 16 * (!nan) + 32 * (!0.0) + 64 * (!-0.0) + 128 * (!1e-300);
    r += 256 * (nan ? 1 : 0) + 512 * (inf ? 1 : 0) + 1024 * (0.0f ? 1 : 0);
    r += 2048 * ((float)u == 18446744073709551616.0f) + 4096 * ((double)s < -9007199254740992.0);
    r += (int)3.99 + (int)-3.99 + (long long)1e18 / 1000000000000000000ll;
    r += (unsigned char)200.5 + (short)-300.7 + (unsigned)4000000000.0 % 7;
    r += (_Bool)0.5 + (_Bool)nan * 10 + (_Bool)0.0 * 100;
    r += -d < 0 && -(-d) > 0 && -0.0 == 0.0;
    for (i = 0; i < 10; i++)
        d += 0.25;
    r += (int)(d * 100);
    {
        float x = 0.1f;
        double y = x;
        r += y == 0.1;                              /* widening keeps the float's value */
        x += 1;
        r += (int)(x * 10);
    }
    r += truth(nan) + truth(-0.0) * 3 + truth(0.5f) * 5 + truth(0.0);
    return r;
}
