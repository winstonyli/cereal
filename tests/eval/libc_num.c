/* tolower, toupper, floor, ceil, frexp in the host library. */
#include <ctype.h>
#include <math.h>

int main(void)
{
    int e = 0, h = 0, c;
    double m;
    for (c = -1; c < 256; c++)
        h = h * 31 + tolower(c) + 7 * toupper(c);
    h = h * 31 + (int)floor(-2.5) + 3 * (int)ceil(-2.5) + 5 * (int)floor(7.0) + 11 * (int)ceil(7.25);
    m = frexp(48.0, &e);
    h = h * 31 + (int)(m * 1000) + e;
    m = frexp(-0.15625, &e);
    h = h * 31 + (int)(m * 1000) + e;
    m = frexp(0.0, &e);
    h = h * 31 + (int)m + e;
    return h & 0x7fffffff;
}
