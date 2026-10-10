// flags: -fdiagnostics-parseable-fixits
#include <stdio.h>
void f(long long x, unsigned y)
{
    printf("%" PRId64 " %" PRIu32 "\n", x, y);
    scanf("%" SCNd64, &x);
}
