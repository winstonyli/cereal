// flags: -std=gnu99 -Wextra
/* A call of a statement expression that is not a function is located at the
 * lone expression (or break/continue) inside it; with several statements or
 * a declaration, at the opening brace.  -Wextra ordered comparisons of a
 * pointer with zero carry [-Wextra]. */
extern void z();
int f(void);
void *p;

void calls(void)
{
    int x = 0;
    for (;;)
        ({break;})();
    for (;;)
        ({continue;})();
    ({ 1; })();
    ({ x; })();
    ({ (x); })();
    ({ x + 1; })();
    ({ x = 2; })();
    ({ f(); })();
    ({ ; })();
    ({ int y = 1; y; })();
    ({ x = 1; x; })();
    (1)();
}

void cmp(void)
{
    if (z >= 0)
        z();
    if (0 >= z)
        z();
    if (p >= (void *)0)
        z();
    if ((void *)0 >= p)
        z();
    if (z >= (void *)0)
        z();
}
