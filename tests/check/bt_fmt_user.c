// flags: -Wall
/* printf found by name in the builtin table; a declaration that carries its
 * own format attribute is checked by that attribute instead */
int printf(const char *, ...);
int myprintf(const char *, ...) __attribute__((format(printf, 1, 2)));

void f(void)
{
    printf("%d\n", "x");
    myprintf("%d\n", "x");
}
