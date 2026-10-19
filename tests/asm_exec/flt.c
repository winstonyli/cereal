/* Floating-point functions for tests/asm_exec.py. */
double dadd(double a, double b)
{
    return a + b * 2.0 - a / 4.0;
}

float fmix(float a, float b)
{
    return a * b - b + 1.5f;
}

double neg(double a)
{
    return -a * 3.0;
}

int cmps(double a, double b)
{
    return (a < b) + 2 * (a <= b) + 4 * (a > b) + 8 * (a >= b) + 16 * (a == b) + 32 * (a != b);
}

int truth(double a, float b)
{
    int r = 0;
    if (a)
        r += 1;
    if (!b)
        r += 2;
    return r + (a && b) * 4 + (a || b) * 8;
}

double conv(short i, short s, unsigned char u, float f)
{
    return i * 0.5 + s + u - f + (double)(unsigned)u;
}

int trunc_(double a, float b)
{
    return (int)a + (int)b * 3 + (char)(a * 2.0) + (unsigned short)(b + 40000.0f);
}

float narrow(double a)
{
    return (float)a * 2;
}

double acc(double x, int n)
{
    double r = 0.0;
    int i;
    for (i = 0; i < n % 6 + 6; i++)
        r += x / (i + 1);
    return r;
}

double inc(double a)
{
    double b = a;
    b++;
    ++b;
    b *= 2.0;
    b -= a--;
    return b + a;
}

double many(double a, int b, double c, int d, double e, int f, double g, int h, double i,
            int j, double k, double l, long m, double n)
{
    return a + b * c - d + e * f - g + h + i * j + k - l + m * n;
}

double callmany(double x, short y)
{
    return many(x, y, x + 1, y + 2, x * 2, y - 3, x - 4, y * 5, x / 2, y + 6, x + 7, x - 8,
                y * 9, x * 0.25) +
           dadd(x, y);
}

int fcallint(double a, short b)
{
    return (int)dadd(a, b) + (int)fmix((float)a, (float)b) + cmps(a, b);
}

int cmp_edge(double a)
{
    double z = a - a;
    return cmps(a, a) + 64 * cmps(a, z / z) + 4096 * cmps(z / z, z / z);
}
