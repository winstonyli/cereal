// flags: -fdiagnostics-generate-patch
#include <stddef.h>
struct point { int xcoord; };
struct point *pp;
int counter;
int f(const char *s)
{
    puts(s);
    return countr + pp.xcoord;
}
