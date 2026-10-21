/* The heap, memory and string functions, abs, and exit with a status. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    int *a = malloc(4 * sizeof *a), *z = calloc(4, sizeof *z), i, sum = 0;
    char s[32], t[32];
    for (i = 0; i < 4; i++)
        a[i] = i * i;
    a = realloc(a, 8 * sizeof *a);
    for (i = 4; i < 8; i++)
        a[i] = i;
    for (i = 0; i < 8; i++)
        sum += a[i];
    printf("sum %d zero %d\n", sum, z[0] + z[3]);
    free(a);
    free(z);
    free(NULL);
    strcpy(s, "hello");
    strcat(s, ", world");
    printf("%s %zu %d %d\n", s, strlen(s), strcmp(s, "hello") > 0, strncmp(s, "help", 3));
    memcpy(t, s, strlen(s) + 1);
    memset(t + 1, 'x', 3);
    printf("%s %d %d\n", t, memcmp(s, t, 5) < 0, memcmp(s, t, 1));
    printf("%s %s %d\n", strchr(s, 'o'), strrchr(s, 'o'), strchr(s, 'z') == NULL);
    memmove(s + 2, s, 5);
    printf("%.7s\n", s);
    memset(t, 'q', 8);
    strncpy(t, "ab", 5);
    printf("%d%d%d %c %zu\n", t[2], t[3], t[4], t[5], strlen(t));
    printf("%d %ld %lld %d\n", abs(-5), labs(-1234567890123L), llabs(-3LL), abs(7));
    printf("%d %d\n", memchr(s, 'w', 12) != NULL, memchr(s, 'z', 12) == NULL);
    exit(sum % 7);
}
