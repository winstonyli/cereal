// flags: -fdiagnostics-parseable-fixits
#include <stddef.h>
#define PAD 1

int f(const char *s)
{
    size_t n = strlen(s);
    printf("%zu\n", n);
    return (int)n + strlen(s);
}
