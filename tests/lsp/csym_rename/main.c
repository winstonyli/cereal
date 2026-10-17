#include "api.h"
#define BODY() level
#define ARG(x) (x)
static int level;
static int scale(int factor)
{
    int r = ARG(factor) * 2;
    return r + level;
}
int run(void)
{
    int total = scale(level);
    total += BODY();
#if 0
    x = gone;
#endif
    return total + api_call();
}
static int featured(void) { return 1; }
static int plat(void) { return 2; }
#ifdef FEATURE_X
int fx = featured();
#endif
#ifdef __APPLE__
int ap = plat();
#endif
