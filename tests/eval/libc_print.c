/* printf and friends: every conversion, flags, width and precision, '*', the string and
 * stream functions, and the value of the calls. */
#include <stdio.h>

int main(void)
{
    char buf[64];
    int n;
    printf("%d %i %u %x %X %o %c|%5d|%-5d|%05d|%+d|% d\n", -5, 7, 4000000000u, 255, 255, 8, 'A', 42, 42, 42, 42, 42);
    printf("%ld %lld %zu %hd %hhd %hhu %lu\n", -1L, 1LL << 40, sizeof(int), 70000, 300, 300, 4000000000ul);
    printf("%s|%10s|%-10s|%.2s|%*d|%.*s|%-*d|\n", "hello", "hi", "hi", "abcdef", 6, 12, 3, "truncate", 4, 7);
    printf("%f %.2f %e %g %G %10.3f %a\n", 3.5, 2.0 / 3, 12345.678, 0.0001, 1e20, -3.14159, 0.5);
    printf("%f %g\n", 1.5f, 2.5f);
    printf("%p %s\n", (void *)0, "100%%");
    printf("100%%\n");
    {
        char u[3] = { 97, 98, 99 };         /* not terminated: a precision stops the read */
        printf("%.3s|%.2s\n", u, u);
    }
    puts("puts line");
    putchar('x');
    putchar('\n');
    fputs("fputs line\n", stdout);
    fprintf(stdout, "fprintf %d\n", 3);
    fprintf(stderr, "to stderr\n");
    n = sprintf(buf, "%d-%s", 12, "ab");
    printf("%d [%s]\n", n, buf);
    n = snprintf(buf, 5, "%s", "truncated");
    printf("%d [%s]\n", n, buf);
    n = snprintf(buf, 64, "%5.1f|%-6s|", 9.96, "ab");
    printf("%d [%s]\n", n, buf);
    n = snprintf(0, 0, "%d", 12345);
    printf("%d\n", n);
    return printf("done\n");
}
