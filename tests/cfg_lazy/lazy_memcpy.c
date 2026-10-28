/* Bytes read by memcpy, not by a typed load (docs/LAZY_BLOBS_DESIGN.md D10): an untouched byte is zero but the
 * solver may set it, so both edges of these guards are reachable under --eval-solve. */
#include <string.h>

unsigned mc_eq(const char *a, const char *b)
{
    unsigned x, y;
    memcpy(&x, a, 4);
    memcpy(&y, b, 4);
    return x == y ? 1 : 0;
}

unsigned mc_const(const unsigned char *a)
{
    unsigned long long x;
    memcpy(&x, a, 8);
    return x == 0x1122334455667788ull ? 1 : 0;
}
