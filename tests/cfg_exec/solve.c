/* Functions guarded by exact values, for tests/eval_sweep.sh: seeded random arguments almost
 * never reach the guarded edge, the solver (--eval-solve, EVAL_SWEEP_SOLVE) does.  Every one is
 * total and free of undefined behaviour, as the other files here. */

int magic(const unsigned char *p)
{
    if (p[0] == 0x42 && p[1] * 3 == 99)
        return 1;
    return 0;
}

int square(int x)
{
    if (x * x == 144)
        return 1;
    return 0;
}

int sumsw(unsigned char a, unsigned char b)
{
    switch (a + b) {
    case 300:
        return 1;
    case 7:
        return 2;
    case 255:
        return 3;
    default:
        return 0;
    }
}

int neighbour(const unsigned char *p)
{
    if (p[3] == (unsigned char)(p[4] + 17))
        return 1;
    return 0;
}

int mixed(unsigned a, unsigned b)
{
    if ((a ^ 0xdeadbeefu) == b * 7u)
        return 1;
    return 0;
}

int chain(int a, int b)
{
    if (a > 1000) {
        if (b < -5000)
            return 2;
        return 1;
    }
    return 0;
}
