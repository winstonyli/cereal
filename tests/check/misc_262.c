// flags: -Wall -Wextra
/* __builtin_-only built-ins (clz, ctz, popcount, bswap32, ...) are typed when
 * called: the result is int/unsigned, not erroneous, so a subscript by it
 * still reads the base (no "set but not used"), and misuse is diagnosed. */
unsigned f(unsigned *bins, int k)
{
    bins[(k > 2 ? __builtin_clz (k - 1) : 0)]++;
    return 1;
}
int *g(unsigned long k, unsigned j)
{
    int *a = __builtin_clzl (k);
    int *b = __builtin_clz (j);
    int *c = __builtin_bswap32 (j);
    return a ? b : c;
}
