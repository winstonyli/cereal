/* Variably modified types keep constant dimensions and the spelling of the
 * variable ones: C99 6.7.5.2p9 (array-5). */
void func(int n, int m)
{
    int a[n][6][m];
    int (*p)[4][n + 1];
    int (*q)[6][m];
    int (*r)[n][7];
    p = a;
    q = a;
    r = a;
}
